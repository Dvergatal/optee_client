/*
 * Copyright (c) 2017-2018, Linaro Limited
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <pkcs11.h>
#include <pkcs11_ta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tee_client_api.h>

#include "pkcs11_processing.h"
#include "invoke_ta.h"
#include "invoke_ta2.h"
#include "serializer.h"
#include "serialize_ck.h"

static struct sks_invoke *ck_session2sks_ctx(CK_SESSION_HANDLE session)
{
	(void)session;
	// TODO: find back the invocation context from the session handle
	// Until we do that, let's use the default invacation context.
	return NULL;
}

/*
 * Helper for input data buffers where caller may refer to user application
 * memory that cannot be registered as shared memory as seen with read-only
 * data.
 */
static TEEC_SharedMemory *register_or_alloc_input_buffer(void *in, size_t len)
{
	TEEC_SharedMemory *shm = NULL;

	if (!in)
		return NULL;

	shm = ckteec_register_shm(in, len, CKTEEC_SHM_IN);
	if (!shm) {
		/*
		 * Input buffer cannot be registered: allocated a
		 * temporary input shared buffer rather than failing.
		 */
		shm = ckteec_alloc_shm(len, CKTEEC_SHM_IN);
		if (!shm)
			return NULL;

		memcpy(shm->buffer, in, len);
	}

	return shm;
}

CK_RV ck_create_object(CK_SESSION_HANDLE session,
			CK_ATTRIBUTE_PTR attribs,
			CK_ULONG count,
			CK_OBJECT_HANDLE_PTR handle)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	struct serializer obj;
	size_t ctrl_size = 0;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *out_shm = NULL;
	uint32_t session_handle = session;
	uint32_t key_handle;
	size_t key_handle_size = sizeof(key_handle);
	char *buf = NULL;
	size_t out_size = 0;

	if (!handle || !attribs)
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_attributes(&obj, attribs, count);
	if (rv)
		goto bail;

	/* Shm io0: (i/o) [session-handle][serialized-attributes] / [status] */
	ctrl_size = sizeof(session_handle) + obj.size;
	ctrl = ckteec_alloc_shm(ctrl_size, CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	buf = ctrl->buffer;

	memcpy(buf, &session_handle, sizeof(session_handle));
	buf += sizeof(session_handle);

	memcpy(buf, obj.buffer, obj.size);

	/* Shm io2: (out) [object handle] */
	out_shm = ckteec_alloc_shm(key_handle_size, CKTEEC_SHM_OUT);
	if (!out_shm) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	rv = ckteec_invoke_ctrl_out(PKCS11_CMD_IMPORT_OBJECT,
				    ctrl, out_shm, &out_size);

	if (rv != CKR_OK || out_size != out_shm->size) {
		if (rv == CKR_OK)
			rv = CKR_DEVICE_ERROR;
		goto bail;
	}

	memcpy(&key_handle, out_shm->buffer, key_handle_size);
	*handle = key_handle;

bail:
	release_serial_object(&obj);
	ckteec_free_shm(out_shm);
	ckteec_free_shm(ctrl);

	return rv;
}

CK_RV ck_destroy_object(CK_SESSION_HANDLE session,
			CK_OBJECT_HANDLE obj)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	size_t ctrl_size = 0;
	char *buf = NULL;
	uint32_t session_handle = session;
	uint32_t obj_id = obj;

	/* Shm io0: (i/o) ctrl = [session-handle][object-handle] / [status] */
	ctrl_size = sizeof(session_handle) + sizeof(obj_id);

	ctrl = ckteec_alloc_shm(ctrl_size, CKTEEC_SHM_INOUT);
	if (!ctrl)
		return CKR_HOST_MEMORY;

	buf = ctrl->buffer;

	memcpy(buf, &session_handle, sizeof(session_handle));
	buf += sizeof(session_handle);

	memcpy(buf, &obj_id, sizeof(obj_id));

	rv = ckteec_invoke_ctrl(PKCS11_CMD_DESTROY_OBJECT, ctrl);

	ckteec_free_shm(ctrl);

	return rv;
}

CK_RV ck_encdecrypt_init(CK_SESSION_HANDLE session,
			 CK_MECHANISM_PTR mechanism,
			 CK_OBJECT_HANDLE key,
			 int decrypt)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	struct serializer obj;
	uint32_t session_handle = session;
	uint32_t key_handle = key;
	size_t ctrl_size = 0;
	char *buf = NULL;

	if (!mechanism)
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_mecha_params(&obj, mechanism);
	if (rv)
		return rv;

	/*
	 * Shm io0: (in/out) ctrl
	 * (in) [session-handle][key-handle][serialized-mechanism-blob]
	 * (out) [status]
	 */
	ctrl_size = sizeof(session_handle) + sizeof(key_handle) + obj.size;

	ctrl = ckteec_alloc_shm(ctrl_size, CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	buf = ctrl->buffer;

	memcpy(buf, &session_handle, sizeof(session_handle));
	buf += sizeof(session_handle);

	memcpy(buf, &key_handle, sizeof(key_handle));
	buf += sizeof(key_handle);

	memcpy(buf, obj.buffer, obj.size);

	rv = ckteec_invoke_ctrl(decrypt ? PKCS11_CMD_DECRYPT_INIT :
				PKCS11_CMD_ENCRYPT_INIT, ctrl);

bail:
	release_serial_object(&obj);
	ckteec_free_shm(ctrl);

	return rv;
}

CK_RV ck_encdecrypt_update(CK_SESSION_HANDLE session,
			   CK_BYTE_PTR in,
			   CK_ULONG in_len,
			   CK_BYTE_PTR out,
			   CK_ULONG_PTR out_len,
			   int decrypt)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *in_shm = NULL;
	TEEC_SharedMemory *out_shm = NULL;
	uint32_t session_handle = session;
	size_t out_size = 0;

	if ((out_len && *out_len && !out) || (in_len && !in))
		return CKR_ARGUMENTS_BAD;

	/* Shm io0: (in/out) ctrl = [session-handle] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(session_handle), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	memcpy(ctrl->buffer, &session_handle, sizeof(session_handle));

	/* Shm io1: input data buffer if any */
	if (in_len) {
		in_shm = register_or_alloc_input_buffer(in, in_len);
		if (!in_shm) {
			rv = CKR_HOST_MEMORY;
			goto bail;
		}
	}

	/* Shm io2: output data buffer */
	if (out_len && *out_len) {
		out_shm = ckteec_register_shm(out, *out_len, CKTEEC_SHM_OUT);
	} else {
		/* Query output data size */
		out_shm = ckteec_alloc_shm(0, CKTEEC_SHM_OUT);
	}

	if (!out_shm) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	/* Invoke */
	rv = ckteec_invoke_ta(decrypt ? PKCS11_CMD_DECRYPT_UPDATE :
			      PKCS11_CMD_ENCRYPT_UPDATE, ctrl,
			      in_shm, NULL, out_shm, &out_size, NULL, NULL);

	if (out_len && (rv == CKR_OK || rv == CKR_BUFFER_TOO_SMALL))
		*out_len = out_size;

bail:
	ckteec_free_shm(out_shm);
	ckteec_free_shm(in_shm);
	ckteec_free_shm(ctrl);

	return rv;
}

CK_RV ck_encdecrypt_oneshot(CK_SESSION_HANDLE session,
			    CK_BYTE_PTR in,
			    CK_ULONG in_len,
			    CK_BYTE_PTR out,
			    CK_ULONG_PTR out_len,
			    int decrypt)
{
	CK_RV rv = CKR_GENERAL_ERROR;
	TEEC_SharedMemory *ctrl = NULL;
	TEEC_SharedMemory *in_shm = NULL;
	TEEC_SharedMemory *out_shm = NULL;
	uint32_t session_handle = session;
	size_t out_size = 0;

	if ((out_len && *out_len && !out) || (in_len && !in))
		return CKR_ARGUMENTS_BAD;

	/* Shm io0: (in/out) ctrl = [session-handle] / [status] */
	ctrl = ckteec_alloc_shm(sizeof(session_handle), CKTEEC_SHM_INOUT);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	memcpy(ctrl->buffer, &session_handle, sizeof(session_handle));

	/* Shm io1: input data buffer */
	if (in_len) {
		in_shm = register_or_alloc_input_buffer(in, in_len);

		if (!in_shm) {
			rv = CKR_HOST_MEMORY;
			goto bail;
		}
	}

	/* Shm io2: output data buffer */
	if (out_len && *out_len) {
		out_shm = ckteec_register_shm(out, *out_len, CKTEEC_SHM_OUT);
	} else {
		/* Query output data size */
		out_shm = ckteec_alloc_shm(0, CKTEEC_SHM_OUT);
	}

	if (!out_shm) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	rv = ckteec_invoke_ta(decrypt ? PKCS11_CMD_DECRYPT_ONESHOT :
			      PKCS11_CMD_ENCRYPT_ONESHOT, ctrl,
			      in_shm, NULL, out_shm, &out_size, NULL, NULL);

	if (out_len && (rv == CKR_OK || rv == CKR_BUFFER_TOO_SMALL))
		*out_len = out_size;

bail:
	ckteec_free_shm(in_shm);
	ckteec_free_shm(out_shm);
	ckteec_free_shm(ctrl);

	return rv;
}

CK_RV ck_encdecrypt_final(CK_SESSION_HANDLE session,
			  CK_BYTE_PTR out,
			  CK_ULONG_PTR out_len,
			  int decrypt)
{
	CK_RV rv;
	uint32_t ctrl;
	size_t ctrl_size;
	void *out_buf = out;
	size_t out_size;

	if (out_len && *out_len && !out)
		return CKR_ARGUMENTS_BAD;

	/* params = [session-handle] */
	ctrl = session;
	ctrl_size = sizeof(ctrl);

	if (!out_len)
		out_size = 0;
	else
		out_size = *out_len;

	rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session), decrypt ?
				 PKCS11_CMD_DECRYPT_FINAL : PKCS11_CMD_ENCRYPT_FINAL,
				 &ctrl, ctrl_size, NULL, 0,
				 out_buf, out_len ? &out_size : NULL);

	if (out_len && (rv == CKR_OK || rv == CKR_BUFFER_TOO_SMALL))
		*out_len = out_size;

	return rv;
}

CK_RV ck_generate_key(CK_SESSION_HANDLE session,
		      CK_MECHANISM_PTR mechanism,
		      CK_ATTRIBUTE_PTR attribs,
		      CK_ULONG count,
		      CK_OBJECT_HANDLE_PTR handle)
{
	CK_RV rv;
	struct serializer smecha;
	struct serializer sattr;
	uint32_t session_handle = session;
	char *ctrl = NULL;
	size_t ctrl_size;
	uint32_t key_handle;
	size_t key_handle_size = sizeof(key_handle);

	if (!handle || (count && !attribs))
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_mecha_params(&smecha, mechanism);
	if (rv)
		return rv;

	rv = serialize_ck_attributes(&sattr, attribs, count);
	if (rv)
		goto bail;

	/* ctrl = [session-handle][serialized-mecha][serialized-attributes] */
	ctrl_size = sizeof(uint32_t) + smecha.size + sattr.size;
	ctrl = malloc(ctrl_size);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	memcpy(ctrl, &session_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t), smecha.buffer, smecha.size);
	memcpy(ctrl + sizeof(uint32_t) + smecha.size, sattr.buffer, sattr.size);

	rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session),
				 PKCS11_CMD_GENERATE_KEY, ctrl, ctrl_size,
				 NULL, 0, &key_handle, &key_handle_size);
	if (rv)
		goto bail;

	*handle = key_handle;

bail:
	free(ctrl);
	release_serial_object(&smecha);
	release_serial_object(&sattr);
	return rv;
}

CK_RV ck_generate_key_pair(CK_SESSION_HANDLE session,
			   CK_MECHANISM_PTR mechanism,
			   CK_ATTRIBUTE_PTR pub_attribs,
			   CK_ULONG pub_count,
			   CK_ATTRIBUTE_PTR priv_attribs,
			   CK_ULONG priv_count,
			   CK_OBJECT_HANDLE_PTR pub_key,
			   CK_OBJECT_HANDLE_PTR priv_key)
{
	CK_RV rv;
	struct serializer smecha;
	struct serializer pub_sattr;
	struct serializer priv_sattr;
	uint32_t session_handle = session;
	char *ctrl = NULL;
	size_t ctrl_size;
	uint32_t key_handle[2];
	size_t key_handle_size = sizeof(key_handle);

	if (!(pub_key && priv_key && pub_attribs && priv_attribs && mechanism))
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_mecha_params(&smecha, mechanism);
	if (rv)
		return rv;

	rv = serialize_ck_attributes(&pub_sattr, pub_attribs, pub_count);
	if (rv)
		goto bail;

	rv = serialize_ck_attributes(&priv_sattr, priv_attribs, priv_count);
	if (rv)
		goto bail;

	/* ctrl = [session-handle][serial-mecha][serial-pub][serial-priv] */
	ctrl_size = sizeof(uint32_t) + smecha.size + pub_sattr.size +
			priv_sattr.size;
	ctrl = malloc(ctrl_size);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	memcpy(ctrl, &session_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t),
		smecha.buffer, smecha.size);
	memcpy(ctrl + sizeof(uint32_t) + smecha.size,
		pub_sattr.buffer, pub_sattr.size);
	memcpy(ctrl + sizeof(uint32_t) + smecha.size + pub_sattr.size,
		priv_sattr.buffer, priv_sattr.size);

	rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session),
				 PKCS11_CMD_GENERATE_KEY_PAIR, ctrl, ctrl_size,
				 NULL, 0, &key_handle[0], &key_handle_size);

	if (key_handle_size != sizeof(key_handle))
		rv = CKR_GENERAL_ERROR;
	if (rv)
		goto bail;

	*pub_key = key_handle[0];
	*priv_key = key_handle[1];

bail:
	free(ctrl);
	release_serial_object(&smecha);
	release_serial_object(&pub_sattr);
	release_serial_object(&priv_sattr);
	return rv;
}

CK_RV ck_signverify_init(CK_SESSION_HANDLE session,
			 CK_MECHANISM_PTR mechanism,
			 CK_OBJECT_HANDLE key,
			 int sign)
{
	CK_RV rv;
	struct serializer obj;
	uint32_t session_handle = session;
	uint32_t key_handle = key;
	char *ctrl = NULL;
	size_t ctrl_size;

	if (!mechanism)
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_mecha_params(&obj, mechanism);
	if (rv)
		return rv;

	/* params = [session-handle][key-handle][serialized-mechanism-blob] */
	ctrl_size = 2 * sizeof(uint32_t) + obj.size;
	ctrl = malloc(ctrl_size);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	memcpy(ctrl, &session_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t), &key_handle, sizeof(uint32_t));
	memcpy(ctrl + 2 * sizeof(uint32_t), obj.buffer, obj.size);

	rv = ck_invoke_ta(ck_session2sks_ctx(session), sign ?
			  PKCS11_CMD_SIGN_INIT : PKCS11_CMD_VERIFY_INIT,
			  ctrl, ctrl_size);

bail:
	free(ctrl);
	release_serial_object(&obj);
	return rv;
}

CK_RV ck_signverify_update(CK_SESSION_HANDLE session,
			   CK_BYTE_PTR in,
			   CK_ULONG in_len,
			   int sign)
{
	CK_RV rv;
	uint32_t ctrl;
	size_t ctrl_size;
	void *in_buf = in;
	size_t in_size = in_len;

	if (in_len && !in)
		return CKR_ARGUMENTS_BAD;

	/* params = [session-handle] */
	ctrl = session;
	ctrl_size = sizeof(ctrl);

	rv = ck_invoke_ta_in(ck_session2sks_ctx(session), sign ?
			     PKCS11_CMD_SIGN_UPDATE : PKCS11_CMD_VERIFY_UPDATE,
			     &ctrl, ctrl_size, in_buf, in_size);

	return rv;
}

CK_RV ck_signverify_oneshot(CK_SESSION_HANDLE session,
			    CK_BYTE_PTR in,
			    CK_ULONG in_len,
			    CK_BYTE_PTR sign_ref,
			    CK_ULONG_PTR sign_len,
			    int sign)
{
	CK_RV rv;
	uint32_t ctrl;
	size_t ctrl_size;
	void *in_buf = in;
	size_t in_size = in_len;
	void *sign_buf = sign_ref;
	size_t sign_size;

	if ((in_size && !in) || (sign_len && *sign_len && !sign_ref))
		return CKR_ARGUMENTS_BAD;

	/* params = [session-handle] */
	ctrl = session;
	ctrl_size = sizeof(ctrl);

	if (!sign_len)
		sign_size = 0;
	else
		sign_size = *sign_len;

	if (sign)
		rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session),
					 PKCS11_CMD_SIGN_ONESHOT,
					 &ctrl, ctrl_size, in_buf, in_size,
					 sign_buf, &sign_size);
	else
		rv = ck_invoke_ta_in_in(ck_session2sks_ctx(session),
					PKCS11_CMD_VERIFY_ONESHOT,
					&ctrl, ctrl_size, in_buf, in_size,
					sign_buf, sign_size);

	if (sign && sign_len && (rv == CKR_OK || rv == CKR_BUFFER_TOO_SMALL))
		*sign_len = sign_size;

	return rv;
}

CK_RV ck_signverify_final(CK_SESSION_HANDLE session,
			  CK_BYTE_PTR sign_ref,
			  CK_ULONG_PTR sign_len,
			  int sign)
{
	CK_RV rv;
	uint32_t ctrl;
	size_t ctrl_size;
	void *sign_buf = sign_ref;
	size_t sign_size = sign_len ? *sign_len : 0;

	if (sign_len && *sign_len && !sign_ref)
		return CKR_ARGUMENTS_BAD;


	/* params = [session-handle] */
	ctrl = session;
	ctrl_size = sizeof(ctrl);

	if (sign)
		rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session),
					 PKCS11_CMD_SIGN_FINAL, &ctrl, ctrl_size,
					 NULL, 0, sign_buf,
					 sign_buf ? &sign_size : NULL);
	else
		rv = ck_invoke_ta_in(ck_session2sks_ctx(session),
				  PKCS11_CMD_VERIFY_FINAL,
				  &ctrl, ctrl_size, sign_buf, sign_size);

	if (sign && sign_len && (rv == CKR_OK || rv == CKR_BUFFER_TOO_SMALL))
		*sign_len = sign_size;

	return rv;
}

CK_RV ck_find_objects_init(CK_SESSION_HANDLE session,
			   CK_ATTRIBUTE_PTR attribs,
			   CK_ULONG count)
{
	CK_RV rv;
	uint32_t session_handle = session;
	struct serializer obj;
	char *ctrl;
	size_t ctrl_size;

	if (count && !attribs)
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_attributes(&obj, attribs, count);
	if (rv)
		return rv;

	/* ctrl = [session-handle][headed-serialized-attributes] */
	ctrl_size = sizeof(uint32_t) + obj.size;
	ctrl = malloc(ctrl_size);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	memcpy(ctrl, &session_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t), obj.buffer, obj.size);

	rv = ck_invoke_ta(ck_session2sks_ctx(session),
			  PKCS11_CMD_FIND_OBJECTS_INIT, ctrl, ctrl_size);

bail:
	release_serial_object(&obj);
	free(ctrl);
	return rv;
}

CK_RV ck_find_objects(CK_SESSION_HANDLE session,
			CK_OBJECT_HANDLE_PTR obj,
			CK_ULONG max_count,
			CK_ULONG_PTR count)

{
	CK_RV rv;
	uint32_t ctrl[1] = { session };
	uint32_t *handles;
	size_t handles_size = max_count * sizeof(uint32_t);
	CK_ULONG n;
	CK_ULONG last;

	if (!count || (*count && !obj))
		return CKR_ARGUMENTS_BAD;

	handles = malloc(handles_size);
	if (!handles)
		return CKR_HOST_MEMORY;

	rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session),
				 PKCS11_CMD_FIND_OBJECTS, ctrl, sizeof(ctrl),
				 NULL, 0, handles, &handles_size);

	if (rv)
		goto bail;

	last = handles_size / sizeof(uint32_t);
	*count = last;

	for (n = 0; n < last; n++) {
		obj[n] = handles[n];
	}

bail:
	free(handles);
	return rv;

}

CK_RV ck_find_objects_final(CK_SESSION_HANDLE session)
{
	CK_RV rv;
	uint32_t ctrl[1] = { session };

	rv = ck_invoke_ta(ck_session2sks_ctx(session),
			  PKCS11_CMD_FIND_OBJECTS_FINAL, ctrl, sizeof(ctrl));

	return rv;
}

CK_RV ck_derive_key(CK_SESSION_HANDLE session,
		    CK_MECHANISM_PTR mechanism,
		    CK_OBJECT_HANDLE parent_handle,
		    CK_ATTRIBUTE_PTR attribs,
		    CK_ULONG count,
		    CK_OBJECT_HANDLE_PTR out_handle)
{
	CK_RV rv;
	struct serializer smecha;
	struct serializer sattr;
	uint32_t session_handle = session;
	char *ctrl = NULL;
	size_t ctrl_size;
	uint32_t parent_key_handle = parent_handle;
	uint32_t key_handle;
	size_t key_handle_size = sizeof(key_handle);

	if (!mechanism || (count && !attribs) || !out_handle)
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_mecha_params(&smecha, mechanism);
	if (rv)
		return rv;

	rv = serialize_ck_attributes(&sattr, attribs, count);
	if (rv)
		goto bail;

	/* ctrl = [session][serialized-mecha][parent-key][key-attributes] */
	ctrl_size = 2 * sizeof(uint32_t) + smecha.size + sattr.size;
	ctrl = malloc(ctrl_size);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	memcpy(ctrl, &session_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t), smecha.buffer, smecha.size);
	memcpy(ctrl + sizeof(uint32_t) + smecha.size,
			&parent_key_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t) + smecha.size + sizeof(uint32_t),
			sattr.buffer, sattr.size);

	rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session),
				 PKCS11_CMD_DERIVE_KEY, ctrl, ctrl_size,
				 NULL, 0, &key_handle, &key_handle_size);
	if (rv)
		goto bail;

	*out_handle = key_handle;

bail:
	free(ctrl);
	release_serial_object(&smecha);
	release_serial_object(&sattr);
	return rv;
}

CK_RV ck_get_attribute_value(CK_SESSION_HANDLE session,
			     CK_OBJECT_HANDLE obj,
			     CK_ATTRIBUTE_PTR attribs,
			     CK_ULONG count)
{
	CK_RV rv;
	struct serializer sattr;
	uint32_t session_handle = session;
	char *ctrl = NULL;
	size_t ctrl_size;
	uint8_t *out = NULL;
	size_t out_size;
	uint32_t obj_handle = obj;
	size_t handle_size = sizeof(obj_handle);

	if (count && !attribs)
		return CKR_ARGUMENTS_BAD;

	rv = serialize_ck_attributes(&sattr, attribs, count);
	if (rv)
		goto bail;

	/* ctrl = [session][obj-handle][attributes] */
	ctrl_size = sizeof(uint32_t) + handle_size + sattr.size;
	ctrl = malloc(ctrl_size);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}
	/* out = [attributes] */
	out_size = sattr.size;
	out = malloc(out_size);
	if (!out){
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	memcpy(ctrl, &session_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t), &obj_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t) + handle_size, sattr.buffer, sattr.size);

	rv = ck_invoke_ta_in_out(ck_session2sks_ctx(session),
				 PKCS11_CMD_GET_ATTRIBUTE_VALUE, ctrl, ctrl_size,
				 NULL, 0, out, &out_size);
	if (rv != CKR_OK && rv != CKR_BUFFER_TOO_SMALL)
		goto bail;

	rv = deserialize_ck_attributes(out, attribs, count);

bail:
	free(ctrl);
	release_serial_object(&sattr);

	return rv;
}
