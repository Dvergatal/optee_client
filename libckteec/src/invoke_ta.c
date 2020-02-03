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
#include "invoke_ta2.h"
#include "local_utils.h"

static struct sks_invoke primary_invoke;

void invoke_ta_open_primary_context(TEEC_Context *context,
				    TEEC_Session *session)
{
	primary_invoke.context = context;
	primary_invoke.session = session;
}

void invoke_ta_close_primary_context(void)
{
	primary_invoke.context = NULL;
	primary_invoke.session = NULL;
}

static struct sks_invoke *get_invoke_context(struct sks_invoke *sks_ctx)
{
	(void)sks_ctx;

	if (!primary_invoke.session)
		return NULL;

	return &primary_invoke;
}

static TEEC_Context *teec_ctx(struct sks_invoke *sks_ctx)
{
	return (TEEC_Context *)sks_ctx->context;
}

static TEEC_Session *teec_sess(struct sks_invoke *sks_ctx)
{
	return (TEEC_Session *)sks_ctx->session;
}

TEEC_SharedMemory *sks_alloc_shm(struct sks_invoke *sks_ctx,
				 size_t size, int in, int out)
{
	struct sks_invoke *ctx = get_invoke_context(sks_ctx);
	TEEC_SharedMemory *shm;

	if (!ctx || (!in && !out))
		return NULL;

	shm = calloc(1, sizeof(TEEC_SharedMemory));
	if (!shm)
		return NULL;

	shm->size = size;

	if (in)
		shm->flags |= TEEC_MEM_INPUT;
	if (out)
		shm->flags |= TEEC_MEM_OUTPUT;

	if (TEEC_AllocateSharedMemory(teec_ctx(ctx), shm)) {
		free(shm);
		return NULL;
	}

	return shm;
}

TEEC_SharedMemory *sks_register_shm(struct sks_invoke *sks_ctx,
				    void *buf, size_t size, int in, int out)
{
	struct sks_invoke *ctx = get_invoke_context(sks_ctx);
	TEEC_SharedMemory *shm;

	if (!ctx || (!in && !out))
		return NULL;

	shm = calloc(1, sizeof(TEEC_SharedMemory));
	if (!shm)
		return NULL;

	shm->buffer = buf;
	shm->size = size;

	if (in)
		shm->flags |= TEEC_MEM_INPUT;
	if (out)
		shm->flags |= TEEC_MEM_OUTPUT;

	if (TEEC_RegisterSharedMemory(teec_ctx(ctx), shm)) {
		free(shm);
		return NULL;
	}

	return shm;
}

void sks_free_shm(TEEC_SharedMemory *shm)
{
	TEEC_ReleaseSharedMemory(shm);
	free(shm);
}

#define DIR_IN			1
#define DIR_OUT			0
#define DIR_NONE		-1

static CK_RV invoke_ta(struct sks_invoke *sks_ctx, unsigned long cmd,
			void *ctrl, size_t ctrl_sz,
			void *io1, size_t *io1_sz, int io1_dir,
			void *io2, size_t *io2_sz, int io2_dir,
			void *io3, size_t *io3_sz, int io3_dir)
{
	struct sks_invoke *ctx = get_invoke_context(sks_ctx);
	uint32_t command = (uint32_t)cmd;
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;
	TEEC_SharedMemory *ctrl_shm = ctrl;
	TEEC_SharedMemory *io1_shm = io1;
	TEEC_SharedMemory *io2_shm = io2;
	TEEC_SharedMemory *io3_shm = io3;
	uint32_t sks_rc;

	memset(&op, 0, sizeof(op));

	/*
	 * Command control field: TEE invocation parameter #0
	 */
	if (ctrl && ctrl_sz) {
		op.params[0].tmpref.buffer = ctrl;
		op.params[0].tmpref.size = ctrl_sz;
		op.paramTypes |= TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INOUT,
						  0, 0, 0);
	}
	if (ctrl && !ctrl_sz) {
		op.params[0].memref.parent = ctrl_shm;
		op.paramTypes |= TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE, 0, 0, 0);
	}

	/*
	 * IO data TEE invocation parameter #1
	 */
	if (io1_sz && (io1_dir == DIR_OUT || (io1_dir == DIR_IN && *io1_sz))) {
		op.params[1].tmpref.buffer = io1;
		op.params[1].tmpref.size = *io1_sz;
		op.paramTypes |= TEEC_PARAM_TYPES(0, io1_dir == DIR_IN ?
						  TEEC_MEMREF_TEMP_INPUT :
						  TEEC_MEMREF_TEMP_OUTPUT,
						  0, 0);
	}
	if (io1_dir != DIR_NONE && !io1_sz && io1) {
		op.params[1].memref.parent = io1_shm;
		op.paramTypes |= TEEC_PARAM_TYPES(0, TEEC_MEMREF_WHOLE, 0, 0);
	}

	/*
	 * IO data TEE invocation parameter #2
	 */
	if (io2_sz && (io2_dir == DIR_OUT || (io2_dir == DIR_IN && *io2_sz))) {
		op.params[2].tmpref.buffer = io2;
		op.params[2].tmpref.size = *io2_sz;
		op.paramTypes |= TEEC_PARAM_TYPES(0, 0, io2_dir == DIR_IN ?
						  TEEC_MEMREF_TEMP_INPUT :
						  TEEC_MEMREF_TEMP_OUTPUT,
						  0);
	}
	if (io2_dir != DIR_NONE && !io2_sz && io2) {
		op.params[2].memref.parent = io2_shm;
		op.paramTypes |= TEEC_PARAM_TYPES(0, 0, TEEC_MEMREF_WHOLE, 0);
	}

	/*
	 * IO data TEE invocation parameter #3
	 */
	if (io3_sz && (io3_dir == DIR_OUT || (io3_dir == DIR_IN && *io3_sz))) {
		op.params[3].tmpref.buffer = io3;
		op.params[3].tmpref.size = *io3_sz;
		op.paramTypes |= TEEC_PARAM_TYPES(0, 0, 0, io3_dir == DIR_IN ?
						  TEEC_MEMREF_TEMP_INPUT :
						  TEEC_MEMREF_TEMP_OUTPUT);
	}
	if (io3_dir != DIR_NONE && !io3_sz && io3) {
		op.params[3].memref.parent = io3_shm;
		op.paramTypes |= TEEC_PARAM_TYPES(0, 0, 0, TEEC_MEMREF_WHOLE);
	}


	/*
	 * Invoke the TEE and update output buffer size on exit.
	 * Too short buffers are treated as positive errors.
	 */
	res = TEEC_InvokeCommand(teec_sess(ctx), command, &op, &origin);

	if (res) {
		if (res == TEEC_ERROR_SHORT_BUFFER) {
			if (io1_dir == DIR_OUT && io1_sz)
				*io1_sz = op.params[1].tmpref.size;
			if (io2_dir == DIR_OUT && io2_sz)
				*io2_sz = op.params[2].tmpref.size;
			if (io3_dir == DIR_OUT && io3_sz)
				*io3_sz = op.params[3].tmpref.size;
		}

		return teec2ck_rv(res);
	}

	/* Get PKCS11 TA return value from ctrl buffer, if none we expect success */
	if (ctrl &&
	    ((ctrl_sz && op.params[0].tmpref.size == sizeof(uint32_t)) ||
	    (!ctrl_sz && op.params[0].memref.size == sizeof(uint32_t)))) {
		memcpy(&sks_rc, ctrl, sizeof(uint32_t));
	} else {
		sks_rc = PKCS11_CKR_OK;
	}

	if (sks_rc == PKCS11_CKR_OK || sks_rc == PKCS11_CKR_BUFFER_TOO_SMALL) {
		if (io1_dir == DIR_OUT && io1_sz)
			*io1_sz = op.params[1].tmpref.size;
		if (io2_dir == DIR_OUT && io2_sz)
			*io2_sz = op.params[2].tmpref.size;
		if (io3_dir == DIR_OUT && io3_sz)
			*io3_sz = op.params[3].tmpref.size;
	}

	return ta2ck_rv(sks_rc);
}

CK_RV ck_invoke_ta(struct sks_invoke *sks_ctx,
		   unsigned long cmd,
		   void *ctrl, size_t ctrl_sz)
{
	return invoke_ta(sks_ctx, cmd, ctrl, ctrl_sz,
			 NULL, NULL, DIR_NONE,
			 NULL, NULL, DIR_NONE,
			 NULL, NULL, DIR_NONE);
}

CK_RV ck_invoke_ta_in(struct sks_invoke *sks_ctx,
		      unsigned long cmd,
		      void *ctrl, size_t ctrl_sz,
		      void *in, size_t in_sz)
{
	return invoke_ta(sks_ctx, cmd, ctrl, ctrl_sz,
			 in, &in_sz, DIR_IN,
			 NULL, NULL, DIR_NONE,
			 NULL, NULL, DIR_NONE);
}


CK_RV ck_invoke_ta_in_out(struct sks_invoke *sks_ctx,
		   unsigned long cmd,
		   void *ctrl, size_t ctrl_sz,
		   void *in, size_t in_sz,
		   void *out, size_t *out_sz)
{
	return invoke_ta(sks_ctx, cmd, ctrl, ctrl_sz,
			 in, &in_sz, DIR_IN,
			 out, out_sz, DIR_OUT,
			 NULL, NULL, DIR_NONE);
}

CK_RV ck_invoke_ta_in_in(struct sks_invoke *sks_ctx,
		   unsigned long cmd,
		   void *ctrl, size_t ctrl_sz,
		   void *in, size_t in_sz,
		   void *in2, size_t in2_sz)
{
	return invoke_ta(sks_ctx, cmd, ctrl, ctrl_sz,
			 in, &in_sz, DIR_IN,
			 in2, &in2_sz, DIR_IN,
			 NULL, NULL, DIR_NONE);
}