// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2017-2020, Linaro Limited
 */

#include <ck_debug.h>
#include <pkcs11.h>
#include <pkcs11_ta.h>
#include <stdlib.h>
#include <string.h>

#include "ck_helpers.h"
#include "invoke_ta.h"
#include "local_utils.h"
#include "pkcs11_token.h"

#define PKCS11_LIB_MANUFACTURER		"Linaro"
#define PKCS11_LIB_DESCRIPTION		"OP-TEE PKCS11 Cryptoki library"

/**
 * ck_get_info - Get local information for C_GetInfo
 */
CK_RV ck_get_info(CK_INFO_PTR info)
{
	const CK_INFO lib_info = {
		.cryptokiVersion = {
			CK_PKCS11_VERSION_MAJOR,
			CK_PKCS11_VERSION_MINOR,
		},
		.manufacturerID = PKCS11_LIB_MANUFACTURER,
		.flags = 0,		/* must be zero per the PKCS#11 2.40 */
		.libraryDescription = PKCS11_LIB_DESCRIPTION,
		.libraryVersion = {
			PKCS11_TA_VERSION_MAJOR,
			PKCS11_TA_VERSION_MINOR
		},
	};
	int n = 0;

	if (!info)
		return CKR_ARGUMENTS_BAD;

	*info = lib_info;

	/* Pad strings with blank characters */
	n = strnlen((char *)info->manufacturerID,
		    sizeof(info->manufacturerID));
	memset(&info->manufacturerID[n], ' ',
	       sizeof(info->manufacturerID) - n);

	n = strnlen((char *)info->libraryDescription,
		    sizeof(info->libraryDescription));
	memset(&info->libraryDescription[n], ' ',
	       sizeof(info->libraryDescription) - n);

	return CKR_OK;
}

/**
 * ck_slot_get_list - Wrap C_GetSlotList into PKCS11_CMD_SLOT_LIST
 */
CK_RV ck_slot_get_list(CK_BBOOL present,
		       CK_SLOT_ID_PTR slots, CK_ULONG_PTR count)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *shm = NULL;
	uint32_t *slot_ids = NULL;
	size_t client_count = 0;
	size_t size = 0;

	/* Discard @present: all slots reported by TA are present */
	(void)present;

	if (!count)
		return CKR_ARGUMENTS_BAD;

	/*
	 * As per spec, if @slots is NULL, "The contents of *pulCount on
	 * entry to C_GetSlotList has no meaning in this case (...)"
	 */
	if (slots)
		client_count = *count;

	size = client_count * sizeof(*slot_ids);

	shm = ckteec_alloc_shm(size, CKTEEC_SHM_OUT);
	if (!shm)
		return CKR_HOST_MEMORY;

	rv = ckteec_invoke_ta(PKCS11_CMD_SLOT_LIST, NULL,
			      NULL, shm, &size, NULL, NULL);

	if (rv == CKR_OK || rv == CKR_BUFFER_TOO_SMALL) {
		/*
		 * When @slot != NULL and @count[0] == 0, library shall
		 * return CKR_BUFFER_TOO_SMALL if provided buffer is too
		 * small whereas TA would have returned CKR_OK since
		 * allocating a zero sized shm instance results in a NULL
		 * shm buffer reference.
		 */
		if (size && slots && !client_count)
			rv = CKR_BUFFER_TOO_SMALL;

		*count = size / sizeof(*slot_ids);

		if (rv == CKR_OK && slots) {
			size_t n = 0;

			slot_ids = shm->buffer;
			for (n = 0; n < *count; n++)
				slots[n] = slot_ids[n];
		}
	}

	ckteec_free_shm(shm);

	return rv;
}

/**
 * ck_slot_get_info - Wrap C_GetSlotInfo into PKCS11_CMD_SLOT_INFO
 */
CK_RV ck_slot_get_info(CK_SLOT_ID slot, CK_SLOT_INFO_PTR info)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *out = NULL;
	uint32_t slot_id = slot;
	struct pkcs11_slot_info *ta_info = NULL;
	size_t out_size = 0;

	if (!info)
		return CKR_ARGUMENTS_BAD;

	ctrl = ckteec_alloc_shm(sizeof(slot_id), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}
	memcpy(ctrl->buffer, &slot_id, sizeof(slot_id));

	out = ckteec_alloc_shm(sizeof(*ta_info), CKTEEC_SHM_OUT);
	if (!out) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}

	rv = ckteec_invoke_ctrl_out(PKCS11_CMD_SLOT_INFO, ctrl, out, &out_size);
	if (rv != CKR_OK || out_size != out->size) {
		if (rv == CKR_OK)
			rv = CKR_DEVICE_ERROR;
		goto out;
	}

	ta_info = out->buffer;

	COMPILE_TIME_ASSERT(sizeof(info->slotDescription) ==
			    sizeof(ta_info->slot_description));
	memcpy(info->slotDescription, ta_info->slot_description,
	       sizeof(info->slotDescription));

	COMPILE_TIME_ASSERT(sizeof(info->manufacturerID) ==
			    sizeof(ta_info->manufacturer_id));
	memcpy(info->manufacturerID, ta_info->manufacturer_id,
	       sizeof(info->manufacturerID));

	info->flags = ta_info->flags;

	COMPILE_TIME_ASSERT(sizeof(info->hardwareVersion) ==
			    sizeof(ta_info->hardware_version));
	memcpy(&info->hardwareVersion, ta_info->hardware_version,
	       sizeof(info->hardwareVersion));

	COMPILE_TIME_ASSERT(sizeof(info->firmwareVersion) ==
			    sizeof(ta_info->firmware_version));
	memcpy(&info->firmwareVersion, ta_info->firmware_version,
	       sizeof(info->firmwareVersion));

out:
	ckteec_free_shm(ctrl);
	ckteec_free_shm(out);

	return rv;
}

/**
 * ck_token_get_info - Wrap C_GetTokenInfo into PKCS11_CMD_TOKEN_INFO
 */
CK_RV ck_token_get_info(CK_SLOT_ID slot, CK_TOKEN_INFO_PTR info)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *out_shm = NULL;
	uint32_t slot_id = slot;
	struct pkcs11_token_info *ta_info = NULL;
	size_t out_size = 0;

	if (!info)
		return CKR_ARGUMENTS_BAD;

	ctrl = ckteec_alloc_shm(sizeof(slot_id), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}
	memcpy(ctrl->buffer, &slot_id, sizeof(slot_id));

	out_shm = ckteec_alloc_shm(sizeof(*ta_info), CKTEEC_SHM_OUT);
	if (!out_shm) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}

	rv = ckteec_invoke_ctrl_out(PKCS11_CMD_TOKEN_INFO, ctrl,
				    out_shm, &out_size);
	if (rv)
		goto out;

	if (out_size != out_shm->size) {
		rv = CKR_DEVICE_ERROR;
		goto out;
	}

	ta_info = out_shm->buffer;

	COMPILE_TIME_ASSERT(sizeof(info->label) == sizeof(ta_info->label));
	memcpy(info->label, ta_info->label, sizeof(info->label));

	COMPILE_TIME_ASSERT(sizeof(info->manufacturerID) ==
			    sizeof(ta_info->manufacturer_id));
	memcpy(info->manufacturerID, ta_info->manufacturer_id,
	       sizeof(info->manufacturerID));

	COMPILE_TIME_ASSERT(sizeof(info->model) == sizeof(ta_info->model));
	memcpy(info->model, ta_info->model, sizeof(info->model));

	COMPILE_TIME_ASSERT(sizeof(info->serialNumber) ==
			    sizeof(ta_info->serial_number));
	memcpy(info->serialNumber, ta_info->serial_number,
	       sizeof(info->serialNumber));

	info->flags = ta_info->flags;
	info->ulMaxSessionCount = ta_info->max_session_count;
	info->ulSessionCount = ta_info->session_count;
	info->ulMaxRwSessionCount = ta_info->max_rw_session_count;
	info->ulRwSessionCount = ta_info->rw_session_count;
	info->ulMaxPinLen = ta_info->max_pin_len;
	info->ulMinPinLen = ta_info->min_pin_len;
	info->ulTotalPublicMemory = ta_info->total_public_memory;
	info->ulFreePublicMemory = ta_info->free_public_memory;
	info->ulTotalPrivateMemory = ta_info->total_private_memory;
	info->ulFreePrivateMemory = ta_info->free_private_memory;

	COMPILE_TIME_ASSERT(sizeof(info->hardwareVersion) ==
			    sizeof(ta_info->hardware_version));
	memcpy(&info->hardwareVersion, ta_info->hardware_version,
	       sizeof(info->hardwareVersion));

	COMPILE_TIME_ASSERT(sizeof(info->firmwareVersion) ==
			    sizeof(ta_info->firmware_version));
	memcpy(&info->firmwareVersion, ta_info->firmware_version,
	       sizeof(info->firmwareVersion));

	COMPILE_TIME_ASSERT(sizeof(info->utcTime) == sizeof(ta_info->utc_time));
	memcpy(&info->utcTime, ta_info->utc_time, sizeof(info->utcTime));

out:
	ckteec_free_shm(ctrl);
	ckteec_free_shm(out_shm);

	return rv;
}

/**
 * ck_init_token - Wrap C_InitToken into PKCS11_CMD_INIT_TOKEN
 */
CK_RV ck_init_token(CK_SLOT_ID slot, CK_UTF8CHAR_PTR pin,
		    CK_ULONG pin_len, CK_UTF8CHAR_PTR label)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	uint32_t slot_id = slot;
	uint32_t pkcs11_pin_len = pin_len;
	size_t ctrl_size = 0;
	char *buf = NULL;

	if (!pin || !label)
		return CKR_ARGUMENTS_BAD;

	/* Shm io0: (in/out) ctrl = [slot-id][pin_len][label][pin] / [status] */
	ctrl_size = sizeof(slot_id) + sizeof(pkcs11_pin_len) + 32 * sizeof(uint8_t) +
		    pkcs11_pin_len;

	ctrl = ckteec_alloc_shm(ctrl_size, CKTEEC_SHM_INOUT);
	if (!ctrl)
		return CKR_HOST_MEMORY;

	buf = ctrl->buffer;

	memcpy(buf, &slot_id, sizeof(slot_id));
	buf += sizeof(slot_id);

	memcpy(buf, &pkcs11_pin_len, sizeof(pkcs11_pin_len));
	buf += sizeof(pkcs11_pin_len);

	memcpy(buf, label, 32 * sizeof(uint8_t));
	buf += 32 * sizeof(uint8_t);

	memcpy(buf, pin, pkcs11_pin_len);

	rv = ckteec_invoke_ctrl(PKCS11_CMD_INIT_TOKEN, ctrl);

	ckteec_free_shm(ctrl);

	return rv;
}

/**
 * ck_token_mechanism_ids - Wrap C_GetMechanismList
 */
CK_RV ck_token_mechanism_ids(CK_SLOT_ID slot,
			     CK_MECHANISM_TYPE_PTR mechanisms,
			     CK_ULONG_PTR count)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *out = NULL;
	uint32_t slot_id = slot;
	size_t out_size = *count * sizeof(uint32_t);

	if (!count || (*count && !mechanisms))
		return CKR_ARGUMENTS_BAD;

	/* Shm io0: (in/out) ctrl = [slot-id] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(slot_id), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	memcpy(ctrl->buffer, &slot_id, sizeof(slot_id));

	/* Shm io2: (out) [mechanism list] */
	if (mechanisms) {
		out = ckteec_register_shm(mechanisms, out_size,
					  CKTEEC_SHM_OUT);
	} else {
		out = ckteec_alloc_shm(out_size, CKTEEC_SHM_OUT);
	}

	if (!out) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	rv = ckteec_invoke_ctrl_out(PKCS11_CMD_MECHANISM_IDS,
				    ctrl, out, &out_size);

	if (rv == CKR_OK || rv == CKR_BUFFER_TOO_SMALL) {
		*count = out_size / sizeof(uint32_t);
	}
	if (!mechanisms && rv == CKR_BUFFER_TOO_SMALL) {
		rv = CKR_OK;
		goto bail;
	}
	if (rv) {
		goto bail;
	}

	if (ta2ck_mechanism_type_list(mechanisms, out->buffer, *count)) {
		LOG_ERROR("unexpected bad mechanism_type list\n");
		rv = CKR_DEVICE_ERROR;
	}

bail:
	ckteec_free_shm(ctrl);
	ckteec_free_shm(out);

	return rv;
}

/**
 * ck_token_mechanism_info - Wrap C_GetMechanismInfo into command MECHANISM_INFO
 */
CK_RV ck_token_mechanism_info(CK_SLOT_ID slot, CK_MECHANISM_TYPE type,
			      CK_MECHANISM_INFO_PTR info)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *out = NULL;
	struct pkcs11_mechanism_info *outbuf;
	uint32_t slot_id = slot;
	uint32_t mecha_type = ck2ta_mechanism_type(type);
	char *buf = NULL;
	size_t out_size = 0;

	if (!info)
		return CKR_ARGUMENTS_BAD;

	mecha_type = ck2ta_mechanism_type(type);
	if (mecha_type == PKCS11_UNDEFINED_ID) {
		LOG_ERROR("mechanism is not support by this library\n");
		return CKR_DEVICE_ERROR;
	}

	/* Shm io0: (in/out) ctrl = [slot-id][mechanism-type] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(slot_id) + sizeof(mecha_type),
				CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	buf = ctrl->buffer;

	memcpy(buf, &slot_id, sizeof(slot_id));
	buf += sizeof(slot_id);

	memcpy(buf, &mecha_type, sizeof(mecha_type));

	/* Shm io2: (out) [mechanism-info] */
	out = ckteec_alloc_shm(sizeof(struct pkcs11_mechanism_info),
			       CKTEEC_SHM_OUT);
	if (!out) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	rv = ckteec_invoke_ctrl_out(PKCS11_CMD_MECHANISM_INFO,
				    ctrl, out, &out_size);

	if (rv != CKR_OK || out_size != out->size) {
		if (rv == CKR_OK)
			rv = CKR_DEVICE_ERROR;
		goto bail;
	}

	outbuf = (struct pkcs11_mechanism_info *)out->buffer;

	if (ta2ck_mechanism_info(info, outbuf))
		rv = CKR_DEVICE_ERROR;

bail:
	ckteec_free_shm(ctrl);
	ckteec_free_shm(out);

	return rv;
}

/**
 * ck_open_session - Wrap C_OpenSession into PKCS11_CMD_OPEN_{RW|RO}_SESSION
 */
CK_RV ck_open_session(CK_SLOT_ID slot, CK_FLAGS flags, CK_VOID_PTR cookie,
		          CK_NOTIFY callback, CK_SESSION_HANDLE_PTR session)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *out = NULL;
	uint32_t slot_id = slot;
	unsigned long cmd;
	uint32_t handle;
	size_t out_size = 0;

	if ((flags & ~(CKF_RW_SESSION | CKF_SERIAL_SESSION)) ||
	    !session)
		return CKR_ARGUMENTS_BAD;

	if (cookie || callback) {
		LOG_ERROR("C_OpenSession does not handle callback yet\n");
		return CKR_FUNCTION_NOT_SUPPORTED;
	}

	/* Shm io0: (in/out) ctrl = [slot-id] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(slot_id), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	memcpy(ctrl->buffer, &slot_id, sizeof(slot_id));

	/* Shm io2: (out) [session handle] */
	out = ckteec_alloc_shm(sizeof(handle), CKTEEC_SHM_OUT);
	if (!out) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	if (flags & CKF_RW_SESSION)
		cmd = PKCS11_CMD_OPEN_RW_SESSION;
	else
		cmd = PKCS11_CMD_OPEN_RO_SESSION;

	rv = ckteec_invoke_ctrl_out(cmd, ctrl, out, &out_size);
	if (rv != CKR_OK || out_size != out->size) {
		if (rv == CKR_OK)
			rv = CKR_DEVICE_ERROR;
		goto bail;
	}

	memcpy(&handle, out->buffer, sizeof(handle));
	*session = handle;

bail:
	ckteec_free_shm(ctrl);
	ckteec_free_shm(out);

	return rv;
}

/**
 * ck_open_session - Wrap C_OpenSession into PKCS11_CMD_CLOSE_SESSION
 */
CK_RV ck_close_session(CK_SESSION_HANDLE session)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	uint32_t session_handle = session;

	/* Shm io0: (in/out) ctrl = [session-handle] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(session_handle), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	memcpy(ctrl->buffer, &session_handle, sizeof(session_handle));

	rv = ckteec_invoke_ctrl(PKCS11_CMD_CLOSE_SESSION, ctrl);

bail:
	ckteec_free_shm(ctrl);

	return rv;
}

/**
 * ck_close_all_sessions - Wrap C_CloseAllSessions into TA command
 */
CK_RV ck_close_all_sessions(CK_SLOT_ID slot)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	uint32_t slot_id = slot;

	/* Shm io0: (in/out) ctrl = [slot-id] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(slot_id), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	memcpy(ctrl->buffer, &slot_id, sizeof(slot_id));

	rv = ckteec_invoke_ctrl(PKCS11_CMD_CLOSE_ALL_SESSIONS, ctrl);

bail:
	ckteec_free_shm(ctrl);

	return rv;
}

/**
 * ck_get_session_info - Wrap C_GetSessionInfo into PKCS11_CMD_SESSION_INFO
 */
CK_RV ck_get_session_info(CK_SESSION_HANDLE session,
			  CK_SESSION_INFO_PTR info)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *out = NULL;
	uint32_t session_handle = session;
	struct pkcs11_session_info *ta_info = NULL;
	size_t out_size = 0;

	if (!info)
		return CKR_ARGUMENTS_BAD;

	/* Shm io0: (in/out) ctrl = [session-handle] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(session_handle), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	memcpy(ctrl->buffer, &session_handle, sizeof(session_handle));

	/* Shm io2: (out) [session info] */
	out = ckteec_alloc_shm(sizeof(struct pkcs11_session_info),
			       CKTEEC_SHM_OUT);
	if (!out) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	rv = ckteec_invoke_ctrl_out(PKCS11_CMD_SESSION_INFO,
				    ctrl, out, &out_size);

	if (rv != CKR_OK || out_size != out->size) {
		if (rv == CKR_OK)
			rv = CKR_DEVICE_ERROR;
		goto bail;
	}

	ta_info = (struct pkcs11_session_info *)out->buffer;
	info->slotID = ta_info->slot_id;
	info->state = ta_info->state;
	info->flags = ta_info->flags;
	info->ulDeviceError = ta_info->device_error;

bail:
	ckteec_free_shm(ctrl);
	ckteec_free_shm(out);

	return rv;
}

/**
 * ck_init_pin - Wrap C_InitPIN into PKCS11_CMD_INIT_PIN
 */
CK_RV ck_init_pin(CK_SESSION_HANDLE session,
		  CK_UTF8CHAR_PTR pin, CK_ULONG pin_len)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	uint32_t pkcs11_session = session;
	uint32_t pkcs11_pin_len = pin_len;
	size_t ctrl_size = 0;
	char *buf = NULL;

	if (!pin)
		return CKR_ARGUMENTS_BAD;

	/* Shm io0: (in/out) ctrl = [session][pin_len][pin] / [status] */
	ctrl_size = sizeof(pkcs11_session) + sizeof(pkcs11_pin_len) +
		    pkcs11_pin_len;

	ctrl = ckteec_alloc_shm(ctrl_size, CKTEEC_SHM_INOUT);
	if (!ctrl)
		return CKR_HOST_MEMORY;

	buf = ctrl->buffer;

	memcpy(buf, &pkcs11_session, sizeof(pkcs11_session));
	buf += sizeof(pkcs11_session);

	memcpy(buf, &pkcs11_pin_len, sizeof(pkcs11_pin_len));
	buf += sizeof(pkcs11_pin_len);

	memcpy(buf, pin, pkcs11_pin_len);

	rv = ckteec_invoke_ctrl(PKCS11_CMD_INIT_PIN, ctrl);

	ckteec_free_shm(ctrl);

	return rv;
}

/**
 * ck_set_pin - Wrap C_SetPIN into PKCS11_CMD_SET_PIN
 */
CK_RV ck_set_pin(CK_SESSION_HANDLE session,
		 CK_UTF8CHAR_PTR old, CK_ULONG old_len,
		 CK_UTF8CHAR_PTR new, CK_ULONG new_len)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	uint32_t pkcs11_session = session;
	uint32_t pkcs11_old_len = old_len;
	uint32_t pkcs11_new_len = new_len;
	size_t ctrl_size = 0;
	char *buf;

	if (!old || !new)
		return CKR_ARGUMENTS_BAD;

	/*
	 * Shm io0: (in/out) ctrl
	 * (in) [session][old_pin_len][new_pin_len][old pin][new pin]
	 * (out) [status]
	 */
	ctrl_size = sizeof(pkcs11_session) + sizeof(pkcs11_old_len) +
		    sizeof(pkcs11_new_len) + pkcs11_old_len + pkcs11_new_len;

	ctrl = ckteec_alloc_shm(ctrl_size, CKTEEC_SHM_INOUT);
	if (!ctrl)
		return CKR_HOST_MEMORY;

	buf = ctrl->buffer;

	memcpy(buf, &pkcs11_session, sizeof(pkcs11_session));
	buf += sizeof(pkcs11_session);

	memcpy(buf, &pkcs11_old_len, sizeof(pkcs11_old_len));
	buf += sizeof(pkcs11_old_len);

	memcpy(buf, &pkcs11_new_len, sizeof(pkcs11_new_len));
	buf += sizeof(pkcs11_new_len);

	memcpy(buf, old, pkcs11_old_len);
	buf += pkcs11_old_len;

	memcpy(buf, new, pkcs11_new_len);

	rv = ckteec_invoke_ctrl(PKCS11_CMD_SET_PIN, ctrl);

	ckteec_free_shm(ctrl);

	return rv;
}

/**
 * ck_login - Wrap C_Login into PKCS11_CMD_LOGIN
 */
CK_RV ck_login(CK_SESSION_HANDLE session, CK_USER_TYPE user_type,
	       CK_UTF8CHAR_PTR pin, CK_ULONG pin_len)

{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	uint32_t pkcs11_session = session;
	uint32_t pkcs11_user = ck2ta_user_type(user_type);
	uint32_t pkcs11_pin_len = pin_len;
	size_t ctrl_size = 0;
	char *buf = NULL;

	if (!pin)
		return CKR_ARGUMENTS_BAD;

	/* Shm io0: (i/o) ctrl = [session][user][pin length][pin] / [status] */
	ctrl_size = sizeof(pkcs11_session) + sizeof(pkcs11_user) +
		    sizeof(pkcs11_pin_len) + pkcs11_pin_len;

	ctrl = ckteec_alloc_shm(ctrl_size, CKTEEC_SHM_INOUT);
	if (!ctrl)
		return CKR_HOST_MEMORY;

	buf = ctrl->buffer;

	memcpy(buf, &pkcs11_session, sizeof(pkcs11_session));
	buf += sizeof(pkcs11_session);

	memcpy(buf, &pkcs11_user, sizeof(pkcs11_user));
	buf += sizeof(pkcs11_user);

	memcpy(buf, &pkcs11_pin_len, sizeof(pkcs11_pin_len));
	buf += sizeof(pkcs11_pin_len);

	memcpy(buf, pin, pkcs11_pin_len);

	rv = ckteec_invoke_ctrl(PKCS11_CMD_LOGIN, ctrl);

	ckteec_free_shm(ctrl);

	return rv;
}

/**
 * ck_logout - Wrap C_Logout into PKCS11_CMD_LOGOUT
 */
CK_RV ck_logout(CK_SESSION_HANDLE session)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	uint32_t session_handle = session;

	/* io0 = [session-handle] */
	ctrl = ckteec_alloc_shm(sizeof(session_handle), CKTEEC_SHM_INOUT);
	if (!ctrl)
		return CKR_HOST_MEMORY;

	memcpy(ctrl->buffer, &session_handle, sizeof(session_handle));

	rv = ckteec_invoke_ctrl(PKCS11_CMD_LOGOUT, ctrl);

	ckteec_free_shm(ctrl);

	return rv;
}
