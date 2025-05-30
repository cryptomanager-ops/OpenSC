/*
 * pkcs15-sec.c: PKCS#15 cryptography functions
 *
 * Copyright (C) 2001, 2002  Juha Yrjölä <juha.yrjola@iki.fi>
 * Copyright (C) 2007        Nils Larsch <nils@larsch.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "internal.h"
#include "pkcs15.h"
#include "pkcs11/pkcs11.h"

static int sec_env_add_param(sc_security_env_t* se, const sc_sec_env_param_t* p)
{
	size_t i;

	if (!se || !p)
	    return SC_ERROR_INCORRECT_PARAMETERS;

	for (i = 0; i < SC_SEC_ENV_MAX_PARAMS; i++) {
	    if (se->params[i].value == NULL) {
		se->params[i] = *p;

		return SC_SUCCESS;
	    }
	}

	return SC_ERROR_TOO_MANY_OBJECTS;
}


static int get_file_path(const struct sc_pkcs15_object* obj, sc_path_t* path)
{
	if (!path)
		return SC_ERROR_INCORRECT_PARAMETERS;

	const struct sc_pkcs15_prkey_info *prkey = (const struct sc_pkcs15_prkey_info *) obj->data;
	const struct sc_pkcs15_skey_info *skey = (const struct sc_pkcs15_skey_info *) obj->data;

	if ((obj->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_PRKEY) {
		*path = prkey->path;
	}
	else if ((obj->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_SKEY) {
		*path = skey->path;
	}
	else
		return SC_ERROR_INCORRECT_PARAMETERS;

	return SC_SUCCESS;
}


static int select_key_file(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *key,
		sc_security_env_t *senv)
{
	sc_context_t *ctx = p15card->card->ctx;
	sc_path_t orig_path;
	sc_path_t path, file_id;
	int r;

	LOG_FUNC_CALLED(ctx);

	LOG_TEST_RET(ctx, get_file_path(key, &orig_path), "Could not get key file path.");
	memset(&path, 0, sizeof(sc_path_t));
	memset(&file_id, 0, sizeof(sc_path_t));

	/* TODO: Why file_app may be NULL -- at least 3F00 has to be present?
	 * Check validity of the following assumption. */
	/* For pkcs15-emulated cards, the file_app may be NULL,
	 * in that case we always assume an absolute path */
	if (!orig_path.len && orig_path.aid.len) {
		/* Private key is a SDO allocated in application DF */
		path = orig_path;
	}
	else if (orig_path.len == 2 && p15card->file_app != NULL) {
		/* Path is relative to app. DF */
		path = p15card->file_app->path;
		file_id = orig_path;
		sc_append_path(&path, &file_id);
		senv->file_ref = file_id;
		senv->flags |= SC_SEC_ENV_FILE_REF_PRESENT;
	}
	else if (orig_path.len > 2) {
		path = orig_path;
		memcpy(file_id.value, orig_path.value + orig_path.len - 2, 2);
		file_id.len = 2;
		file_id.type = SC_PATH_TYPE_FILE_ID;
		senv->file_ref = file_id;
		senv->flags |= SC_SEC_ENV_FILE_REF_PRESENT;
	}
	else {
		LOG_TEST_RET(ctx, SC_ERROR_INVALID_ARGUMENTS, "invalid private key path");
	}

	r = sc_select_file(p15card->card, &path, NULL);
	LOG_TEST_RET(ctx, r, "sc_select_file() failed");

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int use_key(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *obj,
		sc_security_env_t *senv,
		int (*card_command)(sc_card_t *card,
			 const u8 * in, size_t inlen,
			 u8 * out, size_t outlen),
		const u8 * in, size_t inlen, u8 * out, size_t outlen)
{
	int r = SC_SUCCESS;
	int revalidated_cached_pin = 0;
	sc_path_t path;
	LOG_TEST_RET(p15card->card->ctx, get_file_path(obj, &path), "Failed to get key file path.");

	r = sc_lock(p15card->card);
	LOG_TEST_RET(p15card->card->ctx, r, "sc_lock() failed");

	do {
		if (path.len != 0 || path.aid.len != 0) {
			r = select_key_file(p15card, obj, senv);
			if (r < 0) {
				sc_log(p15card->card->ctx,
						"Unable to select private key file");
			}
		}
		if (r == SC_SUCCESS)
			r = sc_set_security_env(p15card->card, senv, 0);

		if (r == SC_SUCCESS)
			r = card_command(p15card->card, in, inlen, out, outlen);

		if (revalidated_cached_pin)
			/* only re-validate once */
			break;
		if (r == SC_ERROR_SECURITY_STATUS_NOT_SATISFIED) {
			r = sc_pkcs15_pincache_revalidate(p15card, obj);
			if (r < 0)
				break;
			revalidated_cached_pin = 1;
		}
	} while (revalidated_cached_pin);

	sc_unlock(p15card->card);

	LOG_FUNC_RETURN(p15card->card->ctx, r);
}

static int format_senv(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *obj,
		sc_security_env_t *senv_out, sc_algorithm_info_t **alg_info_out)
{
	sc_context_t *ctx = p15card->card->ctx;
	const struct sc_pkcs15_prkey_info *prkey = (const struct sc_pkcs15_prkey_info *) obj->data;
	const struct sc_pkcs15_skey_info *skey = (const struct sc_pkcs15_skey_info *) obj->data;

	memset(senv_out, 0, sizeof(*senv_out));

	/* Card driver should have the access to supported algorithms from 'tokenInfo'. So that
	 * it can get value of card specific 'AlgorithmInfo::algRef'. */
	memcpy(senv_out->supported_algos, &p15card->tokeninfo->supported_algos, sizeof(senv_out->supported_algos));

	if (!((obj->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_PRKEY || (obj->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_SKEY))
		LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This is not a private or secret key");

	/* If the key is not native, we can't operate with it. */
	if (!prkey->native)
		LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "This key is not native, cannot operate with it");

	switch (obj->type) {
		case SC_PKCS15_TYPE_PRKEY_RSA:
			*alg_info_out = sc_card_find_rsa_alg(p15card->card, prkey->modulus_length);
			if (*alg_info_out == NULL) {
				sc_log(ctx,
				       "Card does not support RSA with key length %"SC_FORMAT_LEN_SIZE_T"u",
				       prkey->modulus_length);
				LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
			}
			senv_out->algorithm = SC_ALGORITHM_RSA;
			senv_out->key_size_bits = prkey->modulus_length;
			break;

		case SC_PKCS15_TYPE_PRKEY_GOSTR3410:
			*alg_info_out = sc_card_find_gostr3410_alg(p15card->card, prkey->modulus_length);
			if (*alg_info_out == NULL) {
				sc_log(ctx,
				       "Card does not support GOSTR3410 with key length %"SC_FORMAT_LEN_SIZE_T"u",
				       prkey->modulus_length);
				LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
			}
			senv_out->algorithm = SC_ALGORITHM_GOSTR3410;
			senv_out->key_size_bits = prkey->modulus_length;
			break;

		case SC_PKCS15_TYPE_PRKEY_EDDSA:
			*alg_info_out = sc_card_find_eddsa_alg(p15card->card, prkey->field_length, NULL);
			if (*alg_info_out == NULL) {
				sc_log(ctx,
				       "Card does not support EDDSA with field_size %"SC_FORMAT_LEN_SIZE_T"u",
				       prkey->field_length);
				LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
			}
			senv_out->algorithm = SC_ALGORITHM_EDDSA;
			senv_out->key_size_bits = prkey->field_length;
			break;

		case SC_PKCS15_TYPE_PRKEY_XEDDSA:
			*alg_info_out = sc_card_find_xeddsa_alg(p15card->card, prkey->field_length, NULL);
			if (*alg_info_out == NULL) {
				sc_log(ctx,
				       "Card does not support XEDDSA with field_size %"SC_FORMAT_LEN_SIZE_T"u",
				       prkey->field_length);
				LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
			}
			senv_out->algorithm = SC_ALGORITHM_XEDDSA;
			senv_out->key_size_bits = prkey->field_length;
			break;

		case SC_PKCS15_TYPE_PRKEY_EC:
			*alg_info_out = sc_card_find_ec_alg(p15card->card, prkey->field_length, NULL);
			if (*alg_info_out == NULL) {
				sc_log(ctx,
				       "Card does not support EC with field_size %"SC_FORMAT_LEN_SIZE_T"u",
				       prkey->field_length);
				LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
			}
			senv_out->algorithm = SC_ALGORITHM_EC;
			senv_out->key_size_bits = prkey->field_length;

			senv_out->flags |= SC_SEC_ENV_ALG_REF_PRESENT;
			senv_out->algorithm_ref = prkey->field_length;
			break;
		case SC_PKCS15_TYPE_SKEY_GENERIC:
			if (skey->key_type != CKK_AES)
				LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Key type not supported");
			*alg_info_out = sc_card_find_alg(p15card->card, SC_ALGORITHM_AES,
					skey->value_len, NULL);
			if (*alg_info_out == NULL) {
				sc_log(ctx,
				"Card does not support AES with key length %"SC_FORMAT_LEN_SIZE_T"u",
				skey->value_len);
				LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
			}
			senv_out->algorithm = SC_ALGORITHM_AES;
			senv_out->key_size_bits = skey->value_len;
			break;
			/* add other crypto types here */
		default:
			LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Key type not supported");
	}
	senv_out->flags |= SC_SEC_ENV_ALG_PRESENT;

	/* optional keyReference attribute (the default value is -1) */
	if (prkey->key_reference >= 0) {
		senv_out->key_ref_len = 1;
		senv_out->key_ref[0] = prkey->key_reference & 0xFF;
		senv_out->flags |= SC_SEC_ENV_KEY_REF_PRESENT;
	}

	return SC_SUCCESS;
}

int sc_pkcs15_decipher(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *obj,
		unsigned long flags,
		const u8 * in, size_t inlen, u8 *out, size_t outlen, void *pMechanism)
{
	sc_context_t *ctx = p15card->card->ctx;
	int r;
	sc_algorithm_info_t *alg_info = NULL;
	sc_security_env_t senv;
	const struct sc_pkcs15_prkey_info *prkey = (const struct sc_pkcs15_prkey_info *) obj->data;
	unsigned long pad_flags = 0, sec_flags = 0;

	LOG_FUNC_CALLED(ctx);

	if (!(prkey->usage & (SC_PKCS15_PRKEY_USAGE_DECRYPT|SC_PKCS15_PRKEY_USAGE_UNWRAP)))
		LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for decryption");

	r = format_senv(p15card, obj, &senv, &alg_info);
	LOG_TEST_RET(ctx, r, "Could not initialize security environment");
	senv.operation = SC_SEC_OPERATION_DECIPHER;

	r = sc_get_encoding_flags(ctx, flags, alg_info->flags, &pad_flags, &sec_flags);
	LOG_TEST_RET(ctx, r, "cannot encode security operation flags");
	senv.algorithm_flags = sec_flags;

	r = use_key(p15card, obj, &senv, sc_decipher, in, inlen, out,
			outlen);
	LOG_TEST_RET(ctx, r, "use_key() failed");

	/* Strip any padding */
	if (pad_flags & SC_ALGORITHM_RSA_PAD_PKCS1_TYPE_02) {
		unsigned int s = r;
		unsigned int key_size = (unsigned int)alg_info->key_length;
		r = sc_pkcs1_strip_02_padding_constant_time(ctx, key_size / 8, out, s, out, &s);
		/* for keeping PKCS#1 v1.5 depadding constant-time, do not log error here */
	}
#ifdef ENABLE_OPENSSL
	if (pad_flags & SC_ALGORITHM_RSA_PAD_OAEP)
	{
		size_t s = r;
		uint8_t *param = NULL;
		size_t paramlen = 0;
		if (pMechanism != NULL) {
			CK_MECHANISM *mech = (CK_MECHANISM *)pMechanism;
			if (mech->pParameter && sizeof(CK_RSA_PKCS_OAEP_PARAMS) == mech->ulParameterLen) {
				CK_RSA_PKCS_OAEP_PARAMS * oaep_params = mech->pParameter;
				if (oaep_params->source == CKZ_DATA_SPECIFIED) {
					param = oaep_params->pSourceData;
					paramlen = (size_t)oaep_params->ulSourceDataLen;
				}
			}
		}
		r = sc_pkcs1_strip_oaep_padding(ctx, out, s, flags, param, paramlen);
		LOG_TEST_RET(ctx, r, "Invalid OAEP padding");
	}
#endif
	/* do not log error code to prevent side channel attack */
	return r;
}

/* derive one key from another. RSA can use decipher, so this is for only ECDH
 * Since the value may be returned, and the call is expected to provide
 * the buffer, we used the PKCS#11 convention of outlen == 0 and out == NULL
 * to indicate that this is a request for the size.
 * In that case r = 0, and *poutlen = expected size
 */
int sc_pkcs15_derive(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *obj,
		unsigned long flags,
		const u8 * in, size_t inlen, u8 *out,
		size_t *poutlen)
{
	sc_context_t *ctx = p15card->card->ctx;
	int r;
	sc_algorithm_info_t *alg_info = NULL;
	sc_security_env_t senv;
	const struct sc_pkcs15_prkey_info *prkey = (const struct sc_pkcs15_prkey_info *) obj->data;
	unsigned long pad_flags = 0, sec_flags = 0;

	LOG_FUNC_CALLED(ctx);

	if (!(prkey->usage & (SC_PKCS15_PRKEY_USAGE_DERIVE)))
		LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for derivation");

	switch (obj->type) {
		case SC_PKCS15_TYPE_PRKEY_EC:
		case SC_PKCS15_TYPE_PRKEY_XEDDSA:
			if (out == NULL || *poutlen < BYTES4BITS(prkey->field_length)) {
				*poutlen = BYTES4BITS(prkey->field_length);
				r = 0; /* say no data to return */
				LOG_FUNC_RETURN(ctx, r);
			}
			break;
		default:
			LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED,"Key type not supported");
	}

	r = format_senv(p15card, obj, &senv, &alg_info);
	LOG_TEST_RET(ctx, r, "Could not initialize security environment");
	senv.operation = SC_SEC_OPERATION_DERIVE;

	r = sc_get_encoding_flags(ctx, flags, alg_info->flags, &pad_flags, &sec_flags);
	LOG_TEST_RET(ctx, r, "cannot encode security operation flags");
	senv.algorithm_flags = sec_flags;

	r = use_key(p15card, obj, &senv, sc_decipher, in, inlen, out,
			*poutlen);
	LOG_TEST_RET(ctx, r, "use_key() failed");

	/* If card stores derived key on card, then no data is returned
	 * and the key must be used on the card. */
	*poutlen = r;
	LOG_FUNC_RETURN(ctx, r);
}

/*
 * Unwrap a key into a key object on card.
 * in holds the wrapped key data
 * the target file that target_key points to must be created before calling this function
 * Use pkcs15init to perform the complete unwrapping operation and create the pkcs#15 object for the new key.
 */
int sc_pkcs15_unwrap(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *key,
		struct sc_pkcs15_object *target_key,
		unsigned long flags,
		const u8 * in, size_t inlen,
		const u8 * param, size_t paramlen)
{
	sc_context_t *ctx = p15card->card->ctx;
	int r;
	sc_algorithm_info_t *alg_info = NULL;
	sc_security_env_t senv;
	const struct sc_pkcs15_prkey_info *src_prkey = (const struct sc_pkcs15_prkey_info *) key->data;
	const struct sc_pkcs15_skey_info *src_skey = (const struct sc_pkcs15_skey_info *) key->data;
	const struct sc_pkcs15_skey_info *tkey = (const struct sc_pkcs15_skey_info *) target_key->data;
	unsigned long pad_flags = 0, sec_flags = 0;
	u8 *out = 0;
	size_t poutlen = 0;
	sc_path_t path, target_file_id;
	sc_sec_env_param_t senv_param;

	LOG_FUNC_CALLED(ctx);

	if (key->type == SC_PKCS15_TYPE_PRKEY_RSA) {
		if (!(src_prkey->usage & (SC_PKCS15_PRKEY_USAGE_UNWRAP)))
			LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for unwrapping");
	}
	else if ((key->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_SKEY) {
		if (!(src_skey->usage & (SC_PKCS15_PRKEY_USAGE_UNWRAP)))
			LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for unwrapping");
	}
	else
		LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Key type not supported");

	r = format_senv(p15card, key, &senv, &alg_info);
	LOG_TEST_RET(ctx, r, "Could not initialize security environment");
	senv.operation = SC_SEC_OPERATION_UNWRAP;

	memset(&path, 0, sizeof(sc_path_t));
	memset(&target_file_id, 0, sizeof(sc_path_t));

	if (!tkey->path.len && tkey->path.aid.len) {
		/* Target key is a SDO allocated in application DF */
		target_file_id = tkey->path;
	}
	else if (tkey->path.len == 2 && p15card->file_app != NULL) {
		/* Path is relative to app. DF */
		path = p15card->file_app->path;
		target_file_id = tkey->path;
		sc_append_path(&path, &target_file_id);
		target_file_id = path;
	}
	else if (tkey->path.len > 2) {
		path = tkey->path;
		memcpy(target_file_id.value, tkey->path.value + tkey->path.len - 2, 2);
		target_file_id.len = 2;
		target_file_id.type = SC_PATH_TYPE_FILE_ID;
	}
	else {
		LOG_TEST_RET(ctx, SC_ERROR_INVALID_ARGUMENTS, "invalid unwrapping target key path");
	}

	senv_param = (sc_sec_env_param_t) { SC_SEC_ENV_PARAM_TARGET_FILE, &target_file_id, sizeof(target_file_id)};
	LOG_TEST_RET(ctx, sec_env_add_param(&senv, &senv_param), "failed to add target file path to security environment");

	r = sc_get_encoding_flags(ctx, flags, alg_info->flags, &pad_flags, &sec_flags);
	LOG_TEST_RET(ctx, r, "cannot encode security operation flags");
	senv.algorithm_flags = sec_flags;

	if ((sec_flags & (SC_ALGORITHM_AES_CBC | SC_ALGORITHM_AES_CBC_PAD)) > 0) {
	    senv_param = (sc_sec_env_param_t) { SC_SEC_ENV_PARAM_IV, (void*) param, paramlen };
	    LOG_TEST_RET(ctx, sec_env_add_param(&senv, &senv_param), "failed to add IV to security environment");
	}

	r = use_key(p15card, key, &senv, sc_unwrap, in, inlen, out,
		    poutlen);
	LOG_TEST_RET(ctx, r, "use_key() failed");

	LOG_FUNC_RETURN(ctx, r);
}

/*
 * Wrap a key and return a cryptogram
 * <key> is the wrapping key
 * <target_key> is the key to be wrapped
 * wrapped data is returned in <cryptogram>
 */
int sc_pkcs15_wrap(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *key,
		struct sc_pkcs15_object *target_key,
		unsigned long flags,
		u8 *cryptogram, size_t *crgram_len,
		const u8 *param, size_t paramlen) {
	sc_context_t *ctx = p15card->card->ctx;
	int r;
	sc_algorithm_info_t *alg_info = NULL;
	sc_security_env_t senv;
	const struct sc_pkcs15_prkey_info *src_prkey = (const struct sc_pkcs15_prkey_info *) key->data;
	const struct sc_pkcs15_skey_info *src_skey = (const struct sc_pkcs15_skey_info *) key->data;
	const struct sc_pkcs15_prkey_info *target_prkey = (const struct sc_pkcs15_prkey_info *) target_key->data;
	const struct sc_pkcs15_skey_info *target_skey = (const struct sc_pkcs15_skey_info *) target_key->data;
	unsigned long pad_flags = 0, sec_flags = 0;
	sc_path_t tkey_path;
	sc_path_t path, target_file_id;
	sc_sec_env_param_t senv_param;

	LOG_FUNC_CALLED(ctx);

	switch (key->type) {
		case SC_PKCS15_TYPE_PRKEY_RSA:
			if (!(src_prkey->usage & (SC_PKCS15_PRKEY_USAGE_WRAP)))
				LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for wrapping");
			break;
		case SC_PKCS15_TYPE_SKEY_DES:
		case SC_PKCS15_TYPE_SKEY_3DES:
		case SC_PKCS15_TYPE_SKEY_GENERIC:
			if (!(src_skey->usage & (SC_PKCS15_PRKEY_USAGE_WRAP)))
			LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for wrapping");
			break;
		default:
			LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Wrapping key type not supported");
	}

	if (!(target_key->type == SC_PKCS15_TYPE_PRKEY_RSA ||
			(target_key->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_SKEY)) {
		LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Target key type not supported");
	}

	r = format_senv(p15card, key, &senv, &alg_info);
	LOG_TEST_RET(ctx, r, "Could not initialize security environment");
	senv.operation = SC_SEC_OPERATION_WRAP;

	memset(&path, 0, sizeof (sc_path_t));
	memset(&target_file_id, 0, sizeof (sc_path_t));

	switch (target_key->type) {
		case SC_PKCS15_TYPE_PRKEY_RSA:
			tkey_path = target_prkey->path;
			break;
		default: /* we already know it is a secret key */
			tkey_path = target_skey->path;
			break;
	}

	if (!tkey_path.len && tkey_path.aid.len) {
		/* Target key is a SDO allocated in application DF */
		target_file_id = tkey_path;
	} else if (tkey_path.len == 2 && p15card->file_app != NULL) {
		/* Path is relative to app. DF */
		path = p15card->file_app->path;
		target_file_id = tkey_path;
		sc_append_path(&path, &target_file_id);
		target_file_id  = path;
	} else if (tkey_path.len > 2) {
		/*path = tkey_path;*/
		memcpy(target_file_id.value, tkey_path.value + tkey_path.len - 2, 2);
		target_file_id.len = 2;
		target_file_id.type = SC_PATH_TYPE_FILE_ID;
	}
	else {
		LOG_TEST_RET(ctx, SC_ERROR_INVALID_ARGUMENTS, "invalid unwrapping target key path");
	}
	senv_param = (sc_sec_env_param_t) { SC_SEC_ENV_PARAM_TARGET_FILE, &target_file_id, sizeof(target_file_id)};
	LOG_TEST_RET(ctx, sec_env_add_param(&senv, &senv_param), "failed to add target file path to security environment");

	r = sc_get_encoding_flags(ctx, flags, alg_info->flags, &pad_flags, &sec_flags);
	LOG_TEST_RET(ctx, r, "cannot encode security operation flags");
	senv.algorithm_flags = sec_flags;

	if ((sec_flags & (SC_ALGORITHM_AES_CBC | SC_ALGORITHM_AES_CBC_PAD)) > 0) {
		senv_param = (sc_sec_env_param_t) { SC_SEC_ENV_PARAM_IV, (void*) param, paramlen };
		LOG_TEST_RET(ctx, sec_env_add_param(&senv, &senv_param), "failed to add IV to security environment");
	}

	r = use_key(p15card, key, &senv, sc_wrap, NULL, 0, cryptogram, crgram_len ? *crgram_len : 0);

	if (r > -1 && crgram_len) {
		if (*crgram_len < (size_t) r) {
			*crgram_len = r;
			if (cryptogram != NULL) /* if NULL, return success and required buffer length by PKCS#11 convention */
				LOG_TEST_RET(ctx, SC_ERROR_BUFFER_TOO_SMALL, "Buffer too small to hold the wrapped key.");
		}
		*crgram_len = r;
	}

	LOG_FUNC_RETURN(ctx, r);
}


/* copied from pkcs15-cardos.c */
#define USAGE_ANY_SIGN          (SC_PKCS15_PRKEY_USAGE_SIGN|\
                                 SC_PKCS15_PRKEY_USAGE_NONREPUDIATION)
#define USAGE_ANY_DECIPHER      (SC_PKCS15_PRKEY_USAGE_DECRYPT|\
                                 SC_PKCS15_PRKEY_USAGE_UNWRAP)

int sc_pkcs15_compute_signature(struct sc_pkcs15_card *p15card,
				const struct sc_pkcs15_object *obj,
				unsigned long flags, const u8 *in, size_t inlen,
				u8 *out, size_t outlen, void *pMechanism)
{
	sc_context_t *ctx = p15card->card->ctx;
	int r;
	sc_security_env_t senv;
	sc_algorithm_info_t *alg_info;
	const struct sc_pkcs15_prkey_info *prkey = (const struct sc_pkcs15_prkey_info *) obj->data;
	u8 *buf = NULL, *tmp;
	size_t modlen = 0, buflen = 0;
	unsigned long pad_flags = 0, sec_flags = 0;

	LOG_FUNC_CALLED(ctx);

	if (!(prkey->usage & (SC_PKCS15_PRKEY_USAGE_SIGN|SC_PKCS15_PRKEY_USAGE_SIGNRECOVER|
					SC_PKCS15_PRKEY_USAGE_NONREPUDIATION)))
		LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for signing");

	r = format_senv(p15card, obj, &senv, &alg_info);
	LOG_TEST_RET(ctx, r, "Could not initialize security environment");
	senv.operation = SC_SEC_OPERATION_SIGN;

	switch (obj->type) {
		case SC_PKCS15_TYPE_PRKEY_RSA:
			modlen = BYTES4BITS(prkey->modulus_length);
			break;
		case SC_PKCS15_TYPE_PRKEY_GOSTR3410:
			modlen = BYTES4BITS(prkey->modulus_length) * 2;
			break;
		case SC_PKCS15_TYPE_PRKEY_EC:
		case SC_PKCS15_TYPE_PRKEY_EDDSA:
		case SC_PKCS15_TYPE_PRKEY_XEDDSA:
			modlen = BYTES4BITS(prkey->field_length) * 2;  /* 2*nLen */
			break;
		default:
			LOG_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Key type not supported");
	}

	/* Probably never happens, but better make sure */
	if (outlen < modlen)
		LOG_FUNC_RETURN(ctx, SC_ERROR_BUFFER_TOO_SMALL);

	buflen = inlen + modlen;
	buf = sc_mem_secure_alloc(buflen);
	if (buf == NULL)
		LOG_FUNC_RETURN(ctx, SC_ERROR_OUT_OF_MEMORY);
	memcpy(buf, in, inlen);

	/* revert data to sign when signing with the GOST key.
	 * TODO: can it be confirmed by the GOST standard?
	 * TODO: tested with RuTokenECP, has to be validated for RuToken. */
	if (obj->type == SC_PKCS15_TYPE_PRKEY_GOSTR3410) {
		r = sc_mem_reverse(buf, inlen);
		LOG_TEST_GOTO_ERR(ctx, r, "Reverse memory error");
	}

	tmp = buf;

	/* flags: the requested algo
	 * algo_info->flags: what is supported by the card
	 * senv.algorithm_flags: what the card will have to do */

	/* if the card has SC_ALGORITHM_NEED_USAGE set, and the
	 * key is for signing and decryption, we need to emulate signing */

	sc_log(ctx, "supported algorithm flags 0x%lX, private key usage 0x%X", alg_info->flags, prkey->usage);
	if (obj->type == SC_PKCS15_TYPE_PRKEY_RSA) {
		if ((alg_info->flags & SC_ALGORITHM_NEED_USAGE) &&
			((prkey->usage & USAGE_ANY_SIGN) &&
			(prkey->usage & USAGE_ANY_DECIPHER)) ) {
			size_t tmplen = buflen;
			if (flags & SC_ALGORITHM_RSA_RAW) {
				r = sc_pkcs15_decipher(p15card, obj, flags, in, inlen, out, outlen, NULL);
				goto err;
			}
			if (modlen > tmplen)
				LOG_TEST_GOTO_ERR(ctx, SC_ERROR_NOT_ALLOWED, "Buffer too small, needs recompile!");

			/* XXX Assuming RSA key here */
			r = sc_pkcs1_encode(ctx, flags, in, inlen, buf, &tmplen, prkey->modulus_length, pMechanism);

			/* no padding needed - already done */
			flags &= ~SC_ALGORITHM_RSA_PADS;
			/* instead use raw rsa */
			flags |= SC_ALGORITHM_RSA_RAW;

			LOG_TEST_GOTO_ERR(ctx, r, "Unable to add padding");

			r = sc_pkcs15_decipher(p15card, obj, flags, buf, modlen, out, outlen, NULL);
			goto err;
		}


		/* If the card doesn't support the requested algorithm, we normally add the
		 * padding here in software and ask the card to do a raw signature.  There's
		 * one exception to that, where we might be able to get the signature to
		 * succeed by stripping padding if the card only offers higher-level
		 * signature operations.  The only thing we can strip is the DigestInfo
		 * block from PKCS1 padding. */
		if ((flags == (SC_ALGORITHM_RSA_PAD_PKCS1_TYPE_01 | SC_ALGORITHM_RSA_HASH_NONE)) &&
			!(alg_info->flags & SC_ALGORITHM_RSA_RAW) &&
			!(alg_info->flags & SC_ALGORITHM_RSA_HASH_NONE) &&
			(alg_info->flags & SC_ALGORITHM_RSA_PAD_PKCS1_TYPE_01)) {
			unsigned int algo;
			size_t tmplen = buflen;

			r = sc_pkcs1_strip_digest_info_prefix(&algo, tmp, inlen, tmp, &tmplen);
			if (r != SC_SUCCESS || algo == SC_ALGORITHM_RSA_HASH_NONE) {
				r = SC_ERROR_INVALID_DATA;
				goto err;
			}
			flags &= ~SC_ALGORITHM_RSA_HASH_NONE;
			flags |= algo;
			inlen = tmplen;
		}
	}


	/* ECDSA software hash has already been done, or is not needed, or card will do hash */
	/* if card can not do the hash, will use SC_ALGORITHM_ECDSA_RAW */
	if (obj->type == SC_PKCS15_TYPE_PRKEY_EC) {
		if ((alg_info->flags & SC_ALGORITHM_ECDSA_RAW)
				&& !(flags & SC_ALGORITHM_ECDSA_HASHES & alg_info->flags)) {
			sc_log(ctx, "ECDSA using SC_ALGORITHM_ECDSA_RAW flags before 0x%8.8lx", flags);
				flags |= SC_ALGORITHM_ECDSA_RAW;
				flags &= ~SC_ALGORITHM_ECDSA_HASHES;
		}
	}

	r = sc_get_encoding_flags(ctx, flags, alg_info->flags, &pad_flags, &sec_flags);
	if (r != SC_SUCCESS) {
		goto err;
	}
	/* senv now has flags card or driver will do */
	senv.algorithm_flags = sec_flags;

	sc_log(ctx, "DEE flags:0x%8.8lx alg_info->flags:0x%8.8lx pad:0x%8.8lx sec:0x%8.8lx",
		flags, alg_info->flags, pad_flags, sec_flags);

	/* add the padding bytes (if necessary) */
	if (pad_flags != 0) {
		size_t tmplen = buflen;

		/* XXX Assuming RSA key here */
		r = sc_pkcs1_encode(ctx, pad_flags, tmp, inlen, tmp, &tmplen,
		    prkey->modulus_length, pMechanism);
		LOG_TEST_GOTO_ERR(ctx, r, "Unable to add padding");
		inlen = tmplen;
	}
	else if ( senv.algorithm == SC_ALGORITHM_RSA &&
	          (flags & SC_ALGORITHM_RSA_PADS) == SC_ALGORITHM_RSA_PAD_NONE) {
		/* Add zero-padding if input is shorter than the modulus */
		if (inlen < modlen) {
			if (modlen > buflen) {
				r = SC_ERROR_BUFFER_TOO_SMALL;
				goto err;
			}
			memmove(tmp+modlen-inlen, tmp, inlen);
			memset(tmp, 0, modlen-inlen);
		}
		inlen = modlen;
	}
	/* PKCS#11 MECHANISMS V2.30: 6.3.1 EC Signatures
	 * If the length of the hash value is larger than the bit length of n, only
	 * the leftmost bits of the hash up to the length of n will be used. Any
	 * truncation is done by the token.
	 * But if card is going to do the hash, pass in all the data
	 */
	else if (senv.algorithm == SC_ALGORITHM_EC &&
			(senv.algorithm_flags & SC_ALGORITHM_ECDSA_HASHES) == 0) {
		inlen = MIN(inlen, BYTES4BITS(prkey->field_length));
	}


	r = use_key(p15card, obj, &senv, sc_compute_signature, tmp, inlen,
			out, outlen);
	LOG_TEST_GOTO_ERR(ctx, r, "use_key() failed");

	/* Some cards may return RSA signature as integer without leading zero bytes */
	/* Already know outlen >= modlen and r >= 0 */
	if (obj->type == SC_PKCS15_TYPE_PRKEY_RSA && (unsigned)r < modlen) {
		memmove(out + modlen - r, out, r);
		memset(out, 0, modlen - r);
		r = (int)modlen;
	}

err:
	sc_mem_secure_clear_free(buf, buflen);

	LOG_FUNC_RETURN(ctx, r);
}

int
sc_pkcs15_encrypt_sym(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *obj,
		unsigned long flags,
		const u8 *in, size_t inlen, u8 *out, size_t *outlen,
		const u8 *param, size_t paramlen)
{

	sc_context_t *ctx = p15card->card->ctx;

	int i, r;
	sc_algorithm_info_t *alg_info = NULL;
	sc_security_env_t senv;
	sc_sec_env_param_t senv_param;
	const struct sc_pkcs15_skey_info *skey;
	unsigned long pad_flags = 0, sec_flags = 0;
	int revalidated_cached_pin = 0;
	sc_path_t path;

	sc_log(ctx, "called with flags 0x%lX", flags);

	skey = (const struct sc_pkcs15_skey_info *)obj->data;
	if (!(skey->usage & SC_PKCS15_PRKEY_USAGE_ENCRYPT))
		LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for encryption");

	r = format_senv(p15card, obj, &senv, &alg_info);
	LOG_TEST_RET(ctx, r, "Could not initialize security environment");
	senv.operation = SC_SEC_OPERATION_ENCRYPT_SYM;

	r = sc_get_encoding_flags(ctx, flags, alg_info->flags, &pad_flags, &sec_flags);
	LOG_TEST_RET(ctx, r, "cannot encode security operation flags");
	senv.algorithm_flags = sec_flags;

	for (i = 0; i < SC_MAX_SUPPORTED_ALGORITHMS && senv.supported_algos[i].reference; i++) {
		if ((senv.supported_algos[i].mechanism == CKM_AES_ECB && sec_flags == SC_ALGORITHM_AES_ECB) ||
				(senv.supported_algos[i].mechanism == CKM_AES_CBC && sec_flags == SC_ALGORITHM_AES_CBC) ||
				(senv.supported_algos[i].mechanism == CKM_AES_CBC_PAD && sec_flags == SC_ALGORITHM_AES_CBC_PAD)) {
			senv.algorithm_ref = senv.supported_algos[i].algo_ref;
			senv.flags |= SC_SEC_ENV_ALG_REF_PRESENT;
			break;
		}
	}

	if ((sec_flags & (SC_ALGORITHM_AES_CBC | SC_ALGORITHM_AES_CBC_PAD)) > 0) {
		senv_param = (sc_sec_env_param_t){
				SC_SEC_ENV_PARAM_IV, (void *)param, paramlen};
		LOG_TEST_RET(ctx, sec_env_add_param(&senv, &senv_param), "failed to add IV to security environment");
	}

	LOG_TEST_RET(p15card->card->ctx, get_file_path(obj, &path), "Failed to get key file path.");

	LOG_TEST_RET(p15card->card->ctx, r, "sc_lock() failed");

	do {
		r = SC_SUCCESS;
		if (outlen == NULL) {
			/* C_EncryptInit */
			/* select key file and set sec env */
			if (path.len != 0 || path.aid.len != 0) {
				r = select_key_file(p15card, obj, &senv);
				if (r < 0)
					sc_log(p15card->card->ctx, "Unable to select key file");
			}
			if (r == SC_SUCCESS) {
				r = sc_set_security_env(p15card->card, &senv, 0);
				if (r < 0)
					sc_log(p15card->card->ctx, "Unable to set security env");
			}
		}

		if (r == SC_SUCCESS)
			r = sc_encrypt_sym(p15card->card, in, inlen, out, outlen);

		if (revalidated_cached_pin)
			/* only re-validate once */
			break;
		if (r == SC_ERROR_SECURITY_STATUS_NOT_SATISFIED) {
			r = sc_pkcs15_pincache_revalidate(p15card, obj);
			if (r < 0)
				break;
			revalidated_cached_pin = 1;
		}
	} while (revalidated_cached_pin);

	LOG_FUNC_RETURN(ctx, r);
}

int
sc_pkcs15_decrypt_sym(struct sc_pkcs15_card *p15card,
		const struct sc_pkcs15_object *obj,
		unsigned long flags,
		const u8 *in, size_t inlen, u8 *out, size_t *outlen,
		const u8 *param, size_t paramlen)
{

	sc_context_t *ctx = p15card->card->ctx;

	int i, r;
	sc_algorithm_info_t *alg_info = NULL;
	sc_security_env_t senv;
	sc_sec_env_param_t senv_param;
	const struct sc_pkcs15_skey_info *skey;
	unsigned long pad_flags = 0, sec_flags = 0;
	int revalidated_cached_pin = 0;
	sc_path_t path;

	sc_log(ctx, "called with flags 0x%lX", flags);

	skey = (const struct sc_pkcs15_skey_info *)obj->data;
	if (!(skey->usage & SC_PKCS15_PRKEY_USAGE_DECRYPT))
		LOG_TEST_RET(ctx, SC_ERROR_NOT_ALLOWED, "This key cannot be used for encryption");

	r = format_senv(p15card, obj, &senv, &alg_info);
	LOG_TEST_RET(ctx, r, "Could not initialize security environment");
	senv.operation = SC_SEC_OPERATION_DECRYPT_SYM;

	r = sc_get_encoding_flags(ctx, flags, alg_info->flags, &pad_flags, &sec_flags);
	LOG_TEST_RET(ctx, r, "cannot encode security operation flags");
	senv.algorithm_flags = sec_flags;

	for (i = 0; i < SC_MAX_SUPPORTED_ALGORITHMS && senv.supported_algos[i].reference; i++) {
		if ((senv.supported_algos[i].mechanism == CKM_AES_ECB && sec_flags == SC_ALGORITHM_AES_ECB) ||
				(senv.supported_algos[i].mechanism == CKM_AES_CBC && sec_flags == SC_ALGORITHM_AES_CBC) ||
				(senv.supported_algos[i].mechanism == CKM_AES_CBC_PAD && sec_flags == SC_ALGORITHM_AES_CBC_PAD)) {
			senv.algorithm_ref = senv.supported_algos[i].algo_ref;
			senv.flags |= SC_SEC_ENV_ALG_REF_PRESENT;
			break;
		}
	}

	if ((sec_flags & (SC_ALGORITHM_AES_CBC | SC_ALGORITHM_AES_CBC_PAD)) > 0) {
		senv_param = (sc_sec_env_param_t){
				SC_SEC_ENV_PARAM_IV, (void *)param, paramlen};
		LOG_TEST_RET(ctx, sec_env_add_param(&senv, &senv_param), "failed to add IV to security environment");
	}

	LOG_TEST_RET(p15card->card->ctx, get_file_path(obj, &path), "Failed to get key file path.");

	LOG_TEST_RET(p15card->card->ctx, r, "sc_lock() failed");

	do {
		r = SC_SUCCESS;
		if (outlen == NULL) {
			/* C_DecryptInit */
			/* select key file and set sec env */
			if (path.len != 0 || path.aid.len != 0) {
				r = select_key_file(p15card, obj, &senv);
				if (r < 0)
					sc_log(p15card->card->ctx, "Unable to select key file");
			}
			if (r == SC_SUCCESS) {
				r = sc_set_security_env(p15card->card, &senv, 0);
				if (r < 0)
					sc_log(p15card->card->ctx, "Unable to set security env");
			}
		}

		if (r == SC_SUCCESS)
			r = sc_decrypt_sym(p15card->card, in, inlen, out, outlen);

		if (revalidated_cached_pin)
			/* only re-validate once */
			break;
		if (r == SC_ERROR_SECURITY_STATUS_NOT_SATISFIED) {
			r = sc_pkcs15_pincache_revalidate(p15card, obj);
			if (r < 0)
				break;
			revalidated_cached_pin = 1;
		}
	} while (revalidated_cached_pin);

	LOG_FUNC_RETURN(ctx, r);
}
