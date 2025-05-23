/*
 * pkcs11-global.c: PKCS#11 module level functions and function table
 *
 * Copyright (C) 2002  Timo Teräs <timo.teras@iki.fi>
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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef PKCS11_THREAD_LOCKING
#if defined(HAVE_PTHREAD)
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#endif /* PKCS11_THREAD_LOCKING */

#include "sc-pkcs11.h"
#include "ui/notify.h"

#ifdef ENABLE_OPENSSL
#include <openssl/crypto.h>
#include "libopensc/sc-ossl-compat.h"
#endif
#ifdef ENABLE_OPENPACE
#include <eac/eac.h>
#endif

#ifndef MODULE_APP_NAME
#define MODULE_APP_NAME "opensc-pkcs11"
#endif

sc_context_t *context = NULL;
struct sc_pkcs11_config sc_pkcs11_conf;
list_t sessions;
list_t virtual_slots;
#if !defined(_WIN32)
pid_t initialized_pid = (pid_t)-1;
#endif
static int in_finalize = 0;
extern CK_FUNCTION_LIST pkcs11_function_list;
extern CK_FUNCTION_LIST_3_0 pkcs11_function_list_3_0;
int nesting = 0;

#ifdef PKCS11_THREAD_LOCKING

#if defined(HAVE_PTHREAD)

/* mutex used to control C_Initilize creation of mutexes */
static pthread_mutex_t c_initialize_m = PTHREAD_MUTEX_INITIALIZER;
#define C_INITIALIZE_M_LOCK  pthread_mutex_lock(&c_initialize_m);
#define C_INITIALIZE_M_UNLOCK pthread_mutex_unlock(&c_initialize_m);

CK_RV mutex_create(void **mutex)
{
	pthread_mutex_t *m;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return CKR_GENERAL_ERROR;
	pthread_mutex_init(m, NULL);
	*mutex = m;
	return CKR_OK;
}

CK_RV mutex_lock(void *p)
{
	if (pthread_mutex_lock((pthread_mutex_t *) p) == 0)
		return CKR_OK;
	else
		return CKR_GENERAL_ERROR;
}

CK_RV mutex_unlock(void *p)
{
	if (pthread_mutex_unlock((pthread_mutex_t *) p) == 0)
		return CKR_OK;
	else
		return CKR_GENERAL_ERROR;
}

CK_RV mutex_destroy(void *p)
{
	pthread_mutex_destroy((pthread_mutex_t *) p);
	free(p);
	return CKR_OK;
}

static CK_C_INITIALIZE_ARGS _def_locks = {
	mutex_create, mutex_destroy, mutex_lock, mutex_unlock, 0, NULL };
#define HAVE_OS_LOCKING

#elif defined(_WIN32)
CRITICAL_SECTION c_initialize_cs = {0};
#define C_INITIALIZE_M_LOCK EnterCriticalSection(&c_initialize_cs);
#define C_INITIALIZE_M_UNLOCK LeaveCriticalSection(&c_initialize_cs);

CK_RV mutex_create(void **mutex)
{
	CRITICAL_SECTION *m;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return CKR_GENERAL_ERROR;
	InitializeCriticalSection(m);
	*mutex = m;
	return CKR_OK;
}

CK_RV mutex_lock(void *p)
{
	EnterCriticalSection((CRITICAL_SECTION *) p);
	return CKR_OK;
}


CK_RV mutex_unlock(void *p)
{
	LeaveCriticalSection((CRITICAL_SECTION *) p);
	return CKR_OK;
}


CK_RV mutex_destroy(void *p)
{
	DeleteCriticalSection((CRITICAL_SECTION *) p);
	free(p);
	return CKR_OK;
}

static CK_C_INITIALIZE_ARGS _def_locks = {
	mutex_create, mutex_destroy, mutex_lock, mutex_unlock, 0, NULL };
#define HAVE_OS_LOCKING

#endif

#else /* PKCS11_THREAD_LOCKING */
#define C_INITIALIZE_M_LOCK
#define C_INITIALIZE_M_UNLOCK

#endif /* PKCS11_THREAD_LOCKING */

static CK_C_INITIALIZE_ARGS_PTR	global_locking;
static CK_C_INITIALIZE_ARGS app_locking = {
	NULL, NULL, NULL, NULL, 0, NULL };
static void *global_lock = NULL;
#ifdef HAVE_OS_LOCKING
static CK_C_INITIALIZE_ARGS_PTR default_mutex_funcs = &_def_locks;
#else
static CK_C_INITIALIZE_ARGS_PTR default_mutex_funcs = NULL;
#endif

/* wrapper for the locking functions for libopensc */
static int sc_create_mutex(void **m)
{
	if (global_locking == NULL)
		return SC_SUCCESS;
	if (global_locking->CreateMutex(m) == CKR_OK)
		return SC_SUCCESS;
	else
		return SC_ERROR_INTERNAL;
}

static int sc_lock_mutex(void *m)
{
	if (global_locking == NULL)
		return SC_SUCCESS;
	if (global_locking->LockMutex(m) == CKR_OK)
		return SC_SUCCESS;
	else
		return SC_ERROR_INTERNAL;
}

static int sc_unlock_mutex(void *m)
{
	if (global_locking == NULL)
		return SC_SUCCESS;
	if (global_locking->UnlockMutex(m) == CKR_OK)
		return SC_SUCCESS;
	else
		return SC_ERROR_INTERNAL;

}

static int sc_destroy_mutex(void *m)
{
	if (global_locking == NULL)
		return SC_SUCCESS;
	if (global_locking->DestroyMutex(m) == CKR_OK)
		return SC_SUCCESS;
	else
		return SC_ERROR_INTERNAL;
}

static sc_thread_context_t sc_thread_ctx = {
	0, sc_create_mutex, sc_lock_mutex,
	sc_unlock_mutex, sc_destroy_mutex, NULL
};

/* simclist helpers to locate interesting objects by ID */
static int session_list_seeker(const void *el, const void *key) {
	const struct sc_pkcs11_session *session = (struct sc_pkcs11_session *)el;
	if ((el == NULL) || (key == NULL))
		return 0;
	if (session->handle == *(CK_SESSION_HANDLE*)key)
		return 1;
	return 0;
}
static int slot_list_seeker(const void *el, const void *key) {
	const struct sc_pkcs11_slot *slot = (struct sc_pkcs11_slot *)el;
	if ((el == NULL) || (key == NULL))
		return 0;
	if (slot->id == *(CK_SLOT_ID *)key)
		return 1;
	return 0;
}

#ifndef _WIN32
__attribute__((constructor))
#endif
int module_init()
{
#ifdef _WIN32
	InitializeCriticalSection(&c_initialize_cs);
#endif
	sc_notify_init();
	return 1;
}

#ifndef _WIN32
__attribute__((destructor))
#endif
int module_close()
{
	sc_notify_close();
#if defined(ENABLE_OPENSSL) && defined(OPENSSL_SECURE_MALLOC_SIZE) && !defined(LIBRESSL_VERSION_NUMBER)
	CRYPTO_secure_malloc_done();
#endif
#ifdef ENABLE_OPENPACE
	EAC_cleanup();
#endif
#ifdef _WIN32
	DeleteCriticalSection(&c_initialize_cs);
#endif
	return 1;
}

#ifdef _WIN32
BOOL APIENTRY DllMain( HINSTANCE hinstDLL,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		if (!module_init())
			return FALSE;
		break;
	case DLL_PROCESS_DETACH:
		if (lpReserved == NULL) {
			if (!module_close())
				return FALSE;
		}
		break;
	}
	return TRUE;
}
#endif

CK_RV C_Initialize(CK_VOID_PTR pInitArgs)
{
	CK_RV rv;
#if !defined(_WIN32)
	pid_t current_pid;
#endif
	int rc;
	sc_context_param_t ctx_opts;

#if !defined(_WIN32)
	/* Handle fork() exception */
	C_INITIALIZE_M_LOCK
	current_pid = getpid();
	if (current_pid != initialized_pid) {
		if (context && CKR_OK == sc_pkcs11_lock()) {
			context->flags |= SC_CTX_FLAG_TERMINATE;
			sc_pkcs11_unlock();
		}
		C_Finalize(NULL_PTR);
	}
	initialized_pid = current_pid;
	in_finalize = 0;
	C_INITIALIZE_M_UNLOCK
#endif

	/* protect from nesting */
	C_INITIALIZE_M_LOCK
	nesting++;
	if (nesting > 1) {
		nesting--;
		C_INITIALIZE_M_UNLOCK
		return CKR_GENERAL_ERROR;
	}
	C_INITIALIZE_M_UNLOCK
	/* protect from nesting */

	/* protect from multiple threads tryng to setup locking */
	C_INITIALIZE_M_LOCK

	if (context != NULL) {
		if (CKR_OK == sc_pkcs11_lock()) {
			sc_log(context, "C_Initialize(): Cryptoki already initialized\n");
			sc_pkcs11_unlock();
		}
		nesting--;
		C_INITIALIZE_M_UNLOCK
		return CKR_CRYPTOKI_ALREADY_INITIALIZED;
	}

	rv = sc_pkcs11_init_lock((CK_C_INITIALIZE_ARGS_PTR) pInitArgs);
	if (rv != CKR_OK)
		goto out;

	/* set context options */
	memset(&ctx_opts, 0, sizeof(sc_context_param_t));
	ctx_opts.ver        = 0;
	ctx_opts.app_name   = MODULE_APP_NAME;
	ctx_opts.thread_ctx = &sc_thread_ctx;

	rc = sc_context_create(&context, &ctx_opts);
	if (rc != SC_SUCCESS) {
		rv = CKR_GENERAL_ERROR;
		goto out;
	}

	/* Load configuration */
	load_pkcs11_parameters(&sc_pkcs11_conf, context);

	/* List of sessions */
	if (0 != list_init(&sessions)) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}
	list_attributes_seeker(&sessions, session_list_seeker);

	/* List of slots */
	if (0 != list_init(&virtual_slots)) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}
	list_attributes_seeker(&virtual_slots, slot_list_seeker);

	card_detect_all();

out:
	if (context != NULL)
		SC_LOG_RV("C_Initialize() = %s", rv);

	if (rv != CKR_OK) {
		if (context != NULL) {
			sc_release_context(context);
			context = NULL;
		}
		/* Release and destroy the mutex */
		sc_pkcs11_free_lock();
	}

	/* protect from multiple threads tryng to setup locking */
	nesting--;
	C_INITIALIZE_M_UNLOCK

	return rv;
}

CK_RV C_Finalize(CK_VOID_PTR pReserved)
{
	int i;
	void *p;
	sc_pkcs11_slot_t *slot;
	CK_RV rv;

	if (pReserved != NULL_PTR)
		return CKR_ARGUMENTS_BAD;

#if !defined(_WIN32)
	sc_notify_close();
#endif

	if (context == NULL)
		return CKR_CRYPTOKI_NOT_INITIALIZED;

	rv = sc_pkcs11_lock();
	if (rv != CKR_OK)
		return rv;

	sc_log(context, "C_Finalize()");

	/* cancel pending calls */
	in_finalize = 1;
	sc_cancel(context);
	/* remove all cards from readers */
	for (i=0; i < (int)sc_ctx_get_reader_count(context); i++)
		card_removed(sc_ctx_get_reader(context, i));

	while ((p = list_fetch(&sessions)))
		free(p);
	list_destroy(&sessions);

	while ((slot = list_fetch(&virtual_slots))) {
		list_destroy(&slot->objects);
		list_destroy(&slot->logins);
		free(slot);
	}
	list_destroy(&virtual_slots);

	sc_release_context(context);
	context = NULL;

	/* Release and destroy the mutex */
	sc_pkcs11_free_lock();

	return rv;
}

CK_RV get_info_version(CK_INFO_PTR pInfo, CK_VERSION version)
{
	CK_RV rv = CKR_OK;

	if (pInfo == NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	rv = sc_pkcs11_lock();
	if (rv != CKR_OK)
		return rv;

	sc_log(context, "C_GetInfo()");

	memset(pInfo, 0, sizeof(CK_INFO));
	pInfo->cryptokiVersion.major = version.major;
	pInfo->cryptokiVersion.minor = version.minor;
	strcpy_bp(pInfo->manufacturerID,
		  OPENSC_VS_FF_COMPANY_NAME,
		  sizeof(pInfo->manufacturerID));
	strcpy_bp(pInfo->libraryDescription,
		  OPENSC_VS_FF_PRODUCT_NAME,
		  sizeof(pInfo->libraryDescription));
	pInfo->libraryVersion.major = OPENSC_VERSION_MAJOR;
	pInfo->libraryVersion.minor = OPENSC_VERSION_MINOR;

	sc_pkcs11_unlock();
	return rv;
}

CK_RV C_GetInfoV2(CK_INFO_PTR pInfo)
{
	CK_VERSION v = {2, 20};

	return get_info_version(pInfo, v);
}

CK_RV C_GetInfo(CK_INFO_PTR pInfo)
{
	CK_VERSION v = {3, 0};

	return get_info_version(pInfo, v);
}

CK_RV C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR ppFunctionList)
{
	if (ppFunctionList == NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	*ppFunctionList = &pkcs11_function_list;
	return CKR_OK;
}

CK_RV C_GetSlotList(CK_BBOOL       tokenPresent,  /* only slots with token present */
		    CK_SLOT_ID_PTR pSlotList,     /* receives the array of slot IDs */
		    CK_ULONG_PTR   pulCount)      /* receives the number of slots */
{
	CK_SLOT_ID_PTR found = NULL;
	unsigned int i;
	CK_ULONG numMatches;
	sc_pkcs11_slot_t *slot;
	sc_reader_t *prev_reader = NULL;
	CK_RV rv;

	if (pulCount == NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	rv = sc_pkcs11_lock();
	if (rv != CKR_OK)
		return rv;

	sc_log(context, "C_GetSlotList(token=%d, %s)", tokenPresent,
			pSlotList==NULL_PTR? "plug-n-play":"refresh");
	DEBUG_VSS(NULL, "C_GetSlotList before ctx_detect_detect");

	/* Slot list can only change in v2.20 */
	if (pSlotList == NULL_PTR)
		sc_ctx_detect_readers(context);

	DEBUG_VSS(NULL, "C_GetSlotList after ctx_detect_readers");

	card_detect_all();

	if (list_empty(&virtual_slots)) {
		sc_log(context, "returned 0 slots\n");
		*pulCount = 0;
		rv = CKR_OK;
		goto out;
	}

	found = calloc(list_size(&virtual_slots), sizeof(CK_SLOT_ID));

	if (found == NULL) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}

	prev_reader = NULL;
	numMatches = 0;
	for (i=0; i<list_size(&virtual_slots); i++) {
		slot = (sc_pkcs11_slot_t *) list_get_at(&virtual_slots, i);
		/* the list of available slots contains:
		 * - without token(s), at least one empty slot per reader;
		 * - any slot with token;
		 * - any slot that has already been seen;
		 */
		if ((!tokenPresent &&
				(slot->reader != prev_reader ||
			 	 slot->flags & SC_PKCS11_SLOT_FLAG_SEEN))
				|| slot->slot_info.flags & CKF_TOKEN_PRESENT) {
			found[numMatches++] = slot->id;
			slot->flags |= SC_PKCS11_SLOT_FLAG_SEEN;
		}
		prev_reader = slot->reader;
	}
	DEBUG_VSS(NULL, "C_GetSlotList after card_detect_all");

	if (pSlotList == NULL_PTR) {
		sc_log(context, "was only a size inquiry (%lu)\n", numMatches);
		*pulCount = numMatches;
		rv = CKR_OK;
		goto out;
	}
	DEBUG_VSS(NULL, "C_GetSlotList after slot->id reassigned");

	if (*pulCount < numMatches) {
		sc_log(context, "buffer was too small (needed %lu)\n", numMatches);
		*pulCount = numMatches;
		rv = CKR_BUFFER_TOO_SMALL;
		goto out;
	}

	memcpy(pSlotList, found, numMatches * sizeof(CK_SLOT_ID));
	*pulCount = numMatches;
	rv = CKR_OK;

	sc_log(context, "returned %lu slots\n", numMatches);
	DEBUG_VSS(NULL, "Returning a new slot list");

out:
	free (found);
	sc_pkcs11_unlock();
	return rv;
}

static sc_timestamp_t get_current_time(void)
{
#if HAVE_GETTIMEOFDAY
	struct timeval tv;
	struct timezone tz;
	sc_timestamp_t curr;

	if (gettimeofday(&tv, &tz) != 0)
		return 0;

	curr = tv.tv_sec;
	curr *= 1000;
	curr += tv.tv_usec / 1000;
#else
	struct _timeb time_buf;
	sc_timestamp_t curr;

	_ftime(&time_buf);

	curr = time_buf.time;
	curr *= 1000;
	curr += time_buf.millitm;
#endif

	return curr;
}

CK_RV C_GetSlotInfo(CK_SLOT_ID slotID, CK_SLOT_INFO_PTR pInfo)
{
	struct sc_pkcs11_slot *slot = NULL;
	sc_timestamp_t now;
	const char *name;
	CK_RV rv;

	if (pInfo == NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	rv = sc_pkcs11_lock();
	if (rv != CKR_OK)
		return rv;

	sc_log(context, "C_GetSlotInfo(0x%lx)", slotID);

	if (sc_pkcs11_conf.init_sloppy) {
		/* Most likely virtual_slots is empty and has not
		 * been initialized because the caller has *not* called C_GetSlotList
		 * before C_GetSlotInfo, as required by PKCS#11.  Initialize
		 * virtual_slots to make things work and hope the caller knows what
		 * it's doing... */
		card_detect_all();
	}

	rv = slot_get_slot(slotID, &slot);
	DEBUG_VSS(slot, "C_GetSlotInfo found");
	SC_LOG_RV("C_GetSlotInfo() get slot rv %s", rv);
	if (rv == CKR_OK) {
		if (slot->reader == NULL) {
			rv = CKR_TOKEN_NOT_PRESENT;
		} else {
			now = get_current_time();
			if (now >= slot->slot_state_expires || now == 0) {
				/* Update slot status */
				rv = card_detect(slot->reader);
				sc_log(context, "C_GetSlotInfo() card detect rv 0x%lX", rv);

				if (rv == CKR_TOKEN_NOT_RECOGNIZED || rv == CKR_OK)
					slot->slot_info.flags |= CKF_TOKEN_PRESENT;

				/* Don't ask again within the next second */
				slot->slot_state_expires = now + 1000;
			}
		}
	}

	if (rv == CKR_TOKEN_NOT_PRESENT || rv == CKR_TOKEN_NOT_RECOGNIZED)
		rv = CKR_OK;

	if (rv == CKR_OK)
		memcpy(pInfo, &slot->slot_info, sizeof(CK_SLOT_INFO));

	sc_log(context, "C_GetSlotInfo() flags 0x%lX", pInfo->flags);

	name = lookup_enum(RV_T, rv);
	if (name)
		sc_log(context, "C_GetSlotInfo(0x%lx) = %s", slotID, name);
	else
		sc_log(context, "C_GetSlotInfo(0x%lx) = 0x%08lX", slotID, rv);
	sc_pkcs11_unlock();
	return rv;
}

CK_RV C_GetMechanismList(CK_SLOT_ID slotID,
			 CK_MECHANISM_TYPE_PTR pMechanismList,
                         CK_ULONG_PTR pulCount)
{
	struct sc_pkcs11_slot *slot;
	CK_RV rv;

	if (pulCount == NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	rv = sc_pkcs11_lock();
	if (rv != CKR_OK)
		return rv;

	rv = slot_get_token(slotID, &slot);
	if (rv == CKR_OK)
		rv = sc_pkcs11_get_mechanism_list(slot->p11card, pMechanismList, pulCount);

	sc_pkcs11_unlock();
	return rv;
}

CK_RV C_GetMechanismInfo(CK_SLOT_ID slotID,
			 CK_MECHANISM_TYPE type,
			 CK_MECHANISM_INFO_PTR pInfo)
{
	struct sc_pkcs11_slot *slot;
	CK_RV rv;

	if (pInfo == NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	rv = sc_pkcs11_lock();
	if (rv != CKR_OK)
		return rv;

	rv = slot_get_token(slotID, &slot);
	if (rv == CKR_OK)
		rv = sc_pkcs11_get_mechanism_info(slot->p11card, type, pInfo);

	sc_pkcs11_unlock();
	return rv;
}

CK_RV C_InitToken(CK_SLOT_ID slotID,
		  CK_CHAR_PTR pPin,
		  CK_ULONG ulPinLen,
		  CK_CHAR_PTR pLabel)
{
	struct sc_pkcs11_session *session;
	struct sc_pkcs11_slot *slot;
	unsigned char *label, *cpo;
	CK_RV rv;
	unsigned int i;

	/* Strip trailing whitespace and null terminate the label.
	 * Keep the fixed-length buffer though as some other layers or drivers (SC-HSM)
	 * might expect the length is fixed! */
	label = malloc(33);
	if (label == NULL) {
		sc_log(context, "Failed to allocate label memory");
		return CKR_HOST_MEMORY;
	}
	memcpy(label, pLabel, 32);
	label[32] = 0;
	cpo = label + 31;
	while ((cpo >= label) && (*cpo == ' ')) {
		*cpo = 0;
		cpo--;
	}

	sc_log(context, "C_InitToken(pLabel='%s') called", label);
	rv = sc_pkcs11_lock();
	if (rv != CKR_OK) {
		free(label);
		return rv;
	}

	rv = slot_get_token(slotID, &slot);
	if (rv != CKR_OK)   {
		sc_log(context, "C_InitToken() get token error 0x%lX", rv);
		goto out;
	}

	if (!slot->p11card || !slot->p11card->framework
		   || !slot->p11card->framework->init_token) {
		sc_log(context, "C_InitToken() not supported by framework");
		rv = CKR_FUNCTION_NOT_SUPPORTED;
		goto out;
	}

	/* Make sure there's no open session for this token */
	for (i=0; i<list_size(&sessions); i++) {
		session = (struct sc_pkcs11_session*)list_get_at(&sessions, i);
		if (session->slot == slot) {
			rv = CKR_SESSION_EXISTS;
			goto out;
		}
	}

	rv = slot->p11card->framework->init_token(slot, slot->fw_data, pPin, ulPinLen, label);
	if (rv == CKR_OK) {
		/* Now we should re-bind all tokens so they get the
		 * corresponding function vector and flags */
	}

out:
	sc_pkcs11_unlock();
	sc_log(context, "C_InitToken(pLabel='%s') returns 0x%lX", label, rv);
	free(label);
	return rv;
}

CK_RV C_WaitForSlotEvent(CK_FLAGS flags,   /* blocking/nonblocking flag */
			 CK_SLOT_ID_PTR pSlot,  /* location that receives the slot ID */
			 CK_VOID_PTR pReserved) /* reserved.  Should be NULL_PTR */
{
	sc_reader_t *found;
	unsigned int mask, events;
	void *reader_states = NULL;
	CK_SLOT_ID slot_id;
	CK_RV rv;
	int r;

	if (pReserved != NULL_PTR)
		return  CKR_ARGUMENTS_BAD;

	sc_log(context, "C_WaitForSlotEvent(block=%d)", !(flags & CKF_DONT_BLOCK));
#ifndef PCSCLITE_GOOD
	/* Not all pcsc-lite versions implement consistently used functions as they are */
	if (!(flags & CKF_DONT_BLOCK))
		return CKR_FUNCTION_NOT_SUPPORTED;
#endif /* PCSCLITE_GOOD */
	rv = sc_pkcs11_lock();
	if (rv != CKR_OK)
		return rv;

	mask = SC_EVENT_CARD_EVENTS | SC_EVENT_READER_EVENTS;
	/* Detect and add new slots for added readers v2.20 */

	rv = slot_find_changed(&slot_id, mask);
	if ((rv == CKR_OK) || (flags & CKF_DONT_BLOCK))
		goto out;

again:
	sc_log(context, "C_WaitForSlotEvent() reader_states:%p", reader_states);
	sc_pkcs11_unlock();
	r = sc_wait_for_event(context, mask, &found, &events, -1, &reader_states);
	/* Was C_Finalize called ? */
	if (in_finalize == 1)
		return CKR_CRYPTOKI_NOT_INITIALIZED;

	if ((rv = sc_pkcs11_lock()) != CKR_OK)
		return rv;

	if (r != SC_SUCCESS) {
		sc_log(context, "sc_wait_for_event() returned %d\n",  r);
		rv = sc_to_cryptoki_error(r, "C_WaitForSlotEvent");
		goto out;
	}

	/* If no changed slot was found (maybe an unsupported card
	 * was inserted/removed) then go waiting again */
	rv = slot_find_changed(&slot_id, mask);
	if (rv != CKR_OK)
		goto again;

out:
	if (pSlot)
		*pSlot = slot_id;

	/* Free allocated readers states holder */
	if (reader_states)   {
		sc_log(context, "free reader states");
		sc_wait_for_event(context, 0, NULL, NULL, -1, &reader_states);
	}

	SC_LOG_RV("C_WaitForSlotEvent() = %s", rv);
	sc_pkcs11_unlock();
	return rv;
}

/*
 * Interfaces
 */
#define NUM_INTERFACES 2
#define DEFAULT_INTERFACE 0
// clang-format off
CK_INTERFACE interfaces[NUM_INTERFACES] = {
	{"PKCS 11", (void *)&pkcs11_function_list_3_0, 0},
	{"PKCS 11", (void *)&pkcs11_function_list, 0}
};
// clang-format on

CK_RV C_GetInterfaceList(CK_INTERFACE_PTR pInterfacesList,  /* returned interfaces */
			 CK_ULONG_PTR pulCount)         /* number of interfaces returned */
{
	sc_log(context, "C_GetInterfaceList()");

	if (pulCount == NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	if (pInterfacesList == NULL_PTR) {
		*pulCount = NUM_INTERFACES;
		sc_log(context, "was only a size inquiry (%lu)\n", *pulCount);
		return CKR_OK;
	}

	if (*pulCount < NUM_INTERFACES) {
		sc_log(context, "buffer was too small (needed %d)\n", NUM_INTERFACES);
		*pulCount = NUM_INTERFACES;
		return CKR_BUFFER_TOO_SMALL;
	}

	memcpy(pInterfacesList, interfaces, NUM_INTERFACES * sizeof(CK_INTERFACE));
	*pulCount = NUM_INTERFACES;

	sc_log(context, "returned %lu interfaces\n", *pulCount);
	return CKR_OK;
}

CK_RV C_GetInterface(CK_UTF8CHAR_PTR pInterfaceName, /* name of the interface */
                     CK_VERSION_PTR pVersion,       /* version of the interface */
                     CK_INTERFACE_PTR_PTR ppInterface,    /* returned interface */
                     CK_FLAGS flags)           /* flags controlling the semantics
						* of the interface */
{
	int i;

	sc_log(context, "C_GetInterface(%s)",
		pInterfaceName == NULL_PTR ? "<default>" : (char *)pInterfaceName);

	if (ppInterface == NULL) {
		return CKR_ARGUMENTS_BAD;
	}

	if (pInterfaceName == NULL_PTR) {
		/* return default interface */
		*ppInterface = &interfaces[DEFAULT_INTERFACE];
		sc_log(context, "Returning default interface\n");
		return CKR_OK;
	}

	for (i = 0; i < NUM_INTERFACES; i++) {
		CK_VERSION_PTR interface_version = (CK_VERSION_PTR)interfaces[i].pFunctionList;

		/* The interface name is not null here */
		if (strcmp((char *)pInterfaceName, interfaces[i].pInterfaceName) != 0) {
			continue;
		}
		/* If version is not null, it must match */
		if (pVersion != NULL_PTR && (pVersion->major != interface_version->major ||
		    pVersion->minor != interface_version->minor)) {
			continue;
		}
		/* If any flags specified, it must be supported by the interface */
		if ((flags & interfaces[i].flags) != flags) {
			continue;
		}
		*ppInterface = &interfaces[i];
		sc_log(context, "Returning interface %s\n", (*ppInterface)->pInterfaceName);
		return CKR_OK;
	}
	sc_log(context, "Interface not found: %s, version=%d.%d, flags=%lu\n",
		pInterfaceName ? (char *)pInterfaceName : "<null>",
		pVersion ? (*pVersion).major : 0,
		pVersion ? (*pVersion).minor : 0,
		flags);

	return CKR_ARGUMENTS_BAD;
}

/*
 * Locking functions
 */

CK_RV
sc_pkcs11_init_lock(CK_C_INITIALIZE_ARGS_PTR args)
{
	CK_RV rv = CKR_OK;

	int applock = 0;
	int oslock = 0;
	if (global_lock)
		return CKR_OK;

	/* No CK_C_INITIALIZE_ARGS pointer, no locking */
	if (!args)
		return CKR_OK;

	if (args->pReserved != NULL_PTR)
		return CKR_ARGUMENTS_BAD;

	app_locking = *args;

	/* If the app tells us OS locking is okay,
	 * use that. Otherwise use the supplied functions.
	 */
	global_locking = NULL;
	if (args->CreateMutex && args->DestroyMutex &&
		   args->LockMutex   && args->UnlockMutex) {
			applock = 1;
	}
	if ((args->flags & CKF_OS_LOCKING_OK)) {
		oslock = 1;
	}

	/* Based on PKCS#11 v2.11 11.4 */
	if (applock && oslock) {
		/* Shall be used in threaded environment, prefer app provided locking */
		global_locking = &app_locking;
	} else if (!applock && oslock) {
		/* Shall be used in threaded environment, must use operating system locking */
		global_locking = default_mutex_funcs;
	} else if (applock && !oslock) {
		/* Shall be used in threaded environment, must use app provided locking */
		global_locking = &app_locking;
	} else if (!applock && !oslock) {
		/* Shall not be used in threaded environment, use operating system locking */
		global_locking = default_mutex_funcs;
	}

	if (global_locking != NULL) {
		/* create mutex */
		rv = global_locking->CreateMutex(&global_lock);
	}

	return rv;
}

CK_RV sc_pkcs11_lock(void)
{
	if (context == NULL)
		return CKR_CRYPTOKI_NOT_INITIALIZED;

	if (!global_lock)
		return CKR_OK;
	if (global_locking)  {
		while (global_locking->LockMutex(global_lock) != CKR_OK)
			;
	}

	return CKR_OK;
}

static void
__sc_pkcs11_unlock(void *lock)
{
	if (!lock)
		return;
	if (global_locking) {
		while (global_locking->UnlockMutex(lock) != CKR_OK)
			;
	}
}

void sc_pkcs11_unlock(void)
{
	__sc_pkcs11_unlock(global_lock);
}

/*
 * Free the lock - note the lock must be held when
 * you come here
 */
void sc_pkcs11_free_lock(void)
{
	void	*tempLock;

	if (!(tempLock = global_lock))
		return;

	/* Clear the global lock pointer - once we've
	 * unlocked the mutex it's as good as gone */
	global_lock = NULL;

	/* Now unlock. On SMP machines the synchronization
	 * primitives should take care of flushing out
	 * all changed data to RAM */
	__sc_pkcs11_unlock(tempLock);

	if (global_locking)
		global_locking->DestroyMutex(tempLock);
	global_locking = NULL;
}

CK_FUNCTION_LIST pkcs11_function_list = {
	{ 2, 20 }, /* Note: NSS/Firefox ignores this version number and uses C_GetInfo() */
	C_Initialize,
	C_Finalize,
	C_GetInfoV2,
	C_GetFunctionList,
	C_GetSlotList,
	C_GetSlotInfo,
	C_GetTokenInfo,
	C_GetMechanismList,
	C_GetMechanismInfo,
	C_InitToken,
	C_InitPIN,
	C_SetPIN,
	C_OpenSession,
	C_CloseSession,
	C_CloseAllSessions,
	C_GetSessionInfo,
	C_GetOperationState,
	C_SetOperationState,
	C_Login,
	C_Logout,
	C_CreateObject,
	C_CopyObject,
	C_DestroyObject,
	C_GetObjectSize,
	C_GetAttributeValue,
	C_SetAttributeValue,
	C_FindObjectsInit,
	C_FindObjects,
	C_FindObjectsFinal,
	C_EncryptInit,
	C_Encrypt,
	C_EncryptUpdate,
	C_EncryptFinal,
	C_DecryptInit,
	C_Decrypt,
	C_DecryptUpdate,
	C_DecryptFinal,
	C_DigestInit,
	C_Digest,
	C_DigestUpdate,
	C_DigestKey,
	C_DigestFinal,
	C_SignInit,
	C_Sign,
	C_SignUpdate,
	C_SignFinal,
	C_SignRecoverInit,
	C_SignRecover,
	C_VerifyInit,
	C_Verify,
	C_VerifyUpdate,
	C_VerifyFinal,
	C_VerifyRecoverInit,
	C_VerifyRecover,
	C_DigestEncryptUpdate,
	C_DecryptDigestUpdate,
	C_SignEncryptUpdate,
	C_DecryptVerifyUpdate,
	C_GenerateKey,
	C_GenerateKeyPair,
	C_WrapKey,
	C_UnwrapKey,
	C_DeriveKey,
	C_SeedRandom,
	C_GenerateRandom,
	C_GetFunctionStatus,
	C_CancelFunction,
	C_WaitForSlotEvent
};

/* Returned from getInterface */
CK_FUNCTION_LIST_3_0 pkcs11_function_list_3_0 = {
	{ 3, 0 },
	C_Initialize,
	C_Finalize,
	C_GetInfo,
	C_GetFunctionList,
	C_GetSlotList,
	C_GetSlotInfo,
	C_GetTokenInfo,
	C_GetMechanismList,
	C_GetMechanismInfo,
	C_InitToken,
	C_InitPIN,
	C_SetPIN,
	C_OpenSession,
	C_CloseSession,
	C_CloseAllSessions,
	C_GetSessionInfo,
	C_GetOperationState,
	C_SetOperationState,
	C_Login,
	C_Logout,
	C_CreateObject,
	C_CopyObject,
	C_DestroyObject,
	C_GetObjectSize,
	C_GetAttributeValue,
	C_SetAttributeValue,
	C_FindObjectsInit,
	C_FindObjects,
	C_FindObjectsFinal,
	C_EncryptInit,
	C_Encrypt,
	C_EncryptUpdate,
	C_EncryptFinal,
	C_DecryptInit,
	C_Decrypt,
	C_DecryptUpdate,
	C_DecryptFinal,
	C_DigestInit,
	C_Digest,
	C_DigestUpdate,
	C_DigestKey,
	C_DigestFinal,
	C_SignInit,
	C_Sign,
	C_SignUpdate,
	C_SignFinal,
	C_SignRecoverInit,
	C_SignRecover,
	C_VerifyInit,
	C_Verify,
	C_VerifyUpdate,
	C_VerifyFinal,
	C_VerifyRecoverInit,
	C_VerifyRecover,
	C_DigestEncryptUpdate,
	C_DecryptDigestUpdate,
	C_SignEncryptUpdate,
	C_DecryptVerifyUpdate,
	C_GenerateKey,
	C_GenerateKeyPair,
	C_WrapKey,
	C_UnwrapKey,
	C_DeriveKey,
	C_SeedRandom,
	C_GenerateRandom,
	C_GetFunctionStatus,
	C_CancelFunction,
	C_WaitForSlotEvent,
	C_GetInterfaceList,
	C_GetInterface,
	C_LoginUser,
	C_SessionCancel,
	C_MessageEncryptInit,
	C_EncryptMessage,
	C_EncryptMessageBegin,
	C_EncryptMessageNext,
	C_MessageEncryptFinal,
	C_MessageDecryptInit,
	C_DecryptMessage,
	C_DecryptMessageBegin,
	C_DecryptMessageNext,
	C_MessageDecryptFinal,
	C_MessageSignInit,
	C_SignMessage,
	C_SignMessageBegin,
	C_SignMessageNext,
	C_MessageSignFinal,
	C_MessageVerifyInit,
	C_VerifyMessage,
	C_VerifyMessageBegin,
	C_VerifyMessageNext,
	C_MessageVerifyFinal
};
