// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2020, Arm Limited. All rights reserved.
 * Copyright (c) 2019, Linaro Limited
 */

#include <crypto/crypto.h>
#include <ffa.h>
#include <kernel/abort.h>
#include <kernel/secure_partition.h>
#include <kernel/user_mode_ctx.h>
#include <mm/fobj.h>
#include <mm/mobj.h>
#include <mm/tee_mmu.h>
#include <pta_stmm.h>
#include <tee_api_defines_extensions.h>
#include <tee/tee_pobj.h>
#include <tee/tee_svc.h>
#include <tee/tee_svc_storage.h>
#include <zlib.h>

#include "thread_private.h"

static const TEE_UUID stmm_uuid = PTA_STMM_UUID;

/*
 * Once a complete FFA spec is added, these will become discoverable.
 * Until then these are considered part of the internal ABI between
 * OP-TEE and StMM.
 */
static const uint16_t stmm_id = 1U;
static const uint16_t stmm_pta_id = 2U;
static const uint16_t mem_mgr_id = 3U;
static const uint16_t ffa_storage_id = 4U;

static const unsigned int stmm_stack_size = 4 * SMALL_PAGE_SIZE;
static const unsigned int stmm_heap_size = 398 * SMALL_PAGE_SIZE;
static const unsigned int stmm_sec_buf_size = SMALL_PAGE_SIZE;
static const unsigned int stmm_ns_comm_buf_size = SMALL_PAGE_SIZE;

extern unsigned char stmm_image[];
extern const unsigned int stmm_image_size;
extern const unsigned int stmm_image_uncompressed_size;

static struct sec_part_ctx *sec_part_alloc_ctx(const TEE_UUID *uuid)
{
	TEE_Result res = TEE_SUCCESS;
	struct sec_part_ctx *spc = NULL;

	spc = calloc(1, sizeof(*spc));
	if (!spc)
		return NULL;

	spc->ta_ctx.ts_ctx.ops = &secure_partition_ops;
	spc->ta_ctx.ts_ctx.uuid = *uuid;
	spc->ta_ctx.flags = TA_FLAG_SINGLE_INSTANCE |
			    TA_FLAG_INSTANCE_KEEP_ALIVE;
	spc->uctx.ts_ctx = &spc->ta_ctx.ts_ctx;

	res = vm_info_init(&spc->uctx);
	if (res) {
		free(spc);
		return NULL;
	}

	spc->ta_ctx.ref_count = 1;
	condvar_init(&spc->ta_ctx.busy_cv);

	return spc;
}

static void clear_vfp_state(struct sec_part_ctx *spc __maybe_unused)
{
	if (IS_ENABLED(CFG_WITH_VFP))
		thread_user_clear_vfp(&spc->uctx.vfp);
}

static TEE_Result sec_part_enter_user_mode(struct sec_part_ctx *spc)
{
	uint32_t exceptions = 0;
	uint32_t panic_code = 0;
	uint32_t panicked = 0;
	uint64_t cntkctl = 0;

	exceptions = thread_mask_exceptions(THREAD_EXCP_ALL);
	cntkctl = read_cntkctl();
	write_cntkctl(cntkctl | CNTKCTL_PL0PCTEN);
	__thread_enter_user_mode(&spc->regs, &panicked, &panic_code);
	write_cntkctl(cntkctl);
	thread_unmask_exceptions(exceptions);

	clear_vfp_state(spc);

	if (panicked) {
		abort_print_current_ta();
		DMSG("sec_part panicked with code %#"PRIx32, panic_code);
		return TEE_ERROR_TARGET_DEAD;
	}

	return TEE_SUCCESS;
}

static void init_stmm_regs(struct sec_part_ctx *spc, unsigned long a0,
			   unsigned long a1, unsigned long sp, unsigned long pc)
{
	spc->regs.x[0] = a0;
	spc->regs.x[1] = a1;
	spc->regs.sp = sp;
	spc->regs.pc = pc;
}

static TEE_Result alloc_and_map_sp_fobj(struct sec_part_ctx *spc, size_t sz,
					uint32_t prot, vaddr_t *va)
{
	size_t num_pgs = ROUNDUP(sz, SMALL_PAGE_SIZE) / SMALL_PAGE_SIZE;
	struct fobj *fobj = fobj_ta_mem_alloc(num_pgs);
	struct mobj *mobj = mobj_with_fobj_alloc(fobj, NULL);
	TEE_Result res = TEE_SUCCESS;

	fobj_put(fobj);
	if (!mobj)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = vm_map(&spc->uctx, va, num_pgs * SMALL_PAGE_SIZE,
		     prot, 0, mobj, 0);
	if (res)
		mobj_put(mobj);

	return TEE_SUCCESS;
}

static void *zalloc(void *opaque __unused, unsigned int items,
		    unsigned int size)
{
	return malloc(items * size);
}

static void zfree(void *opaque __unused, void *address)
{
	free(address);
}

static void uncompress_image(void *dst, size_t dst_size, void *src,
			     size_t src_size)
{
	z_stream strm = {
		.next_in = src,
		.avail_in = src_size,
		.next_out = dst,
		.avail_out = dst_size,
		.zalloc = zalloc,
		.zfree = zfree,
	};

	if (inflateInit(&strm) != Z_OK)
		panic("inflateInit");

	if (inflate(&strm, Z_SYNC_FLUSH) != Z_STREAM_END)
		panic("inflate");

	if (inflateEnd(&strm) != Z_OK)
		panic("inflateEnd");
}

static TEE_Result load_stmm(struct sec_part_ctx *spc)
{
	struct secure_partition_boot_info *boot_info = NULL;
	struct secure_partition_mp_info *mp_info = NULL;
	TEE_Result res = TEE_SUCCESS;
	vaddr_t sp_addr = 0;
	vaddr_t image_addr = 0;
	vaddr_t heap_addr = 0;
	vaddr_t stack_addr = 0;
	vaddr_t sec_buf_addr = 0;
	vaddr_t comm_buf_addr = 0;
	unsigned int sp_size = 0;
	unsigned int uncompressed_size_roundup = 0;

	uncompressed_size_roundup = ROUNDUP(stmm_image_uncompressed_size,
					    SMALL_PAGE_SIZE);
	sp_size = uncompressed_size_roundup + stmm_stack_size +
		  stmm_heap_size + stmm_sec_buf_size;
	res = alloc_and_map_sp_fobj(spc, sp_size,
				    TEE_MATTR_PRW, &sp_addr);
	if (res)
		return res;

	res = alloc_and_map_sp_fobj(spc, stmm_ns_comm_buf_size,
				    TEE_MATTR_URW | TEE_MATTR_PRW,
				    &comm_buf_addr);
	/*
	 * We don't need to free the previous instance here, they'll all be
	 * handled during the destruction call (sec_part_ctx_destroy())
	 */
	if (res)
		return res;

	image_addr = sp_addr;
	heap_addr = image_addr + uncompressed_size_roundup;
	stack_addr = heap_addr + stmm_heap_size;
	sec_buf_addr = stack_addr + stmm_stack_size;

	tee_mmu_set_ctx(&spc->ta_ctx.ts_ctx);
	uncompress_image((void *)image_addr, stmm_image_uncompressed_size,
			 stmm_image, stmm_image_size);

	res = vm_set_prot(&spc->uctx, image_addr, uncompressed_size_roundup,
			  TEE_MATTR_URX | TEE_MATTR_PR);
	if (res)
		return res;

	res = vm_set_prot(&spc->uctx, heap_addr, stmm_heap_size,
			  TEE_MATTR_URW | TEE_MATTR_PRW);
	if (res)
		return res;

	res = vm_set_prot(&spc->uctx, stack_addr, stmm_stack_size,
			  TEE_MATTR_URW | TEE_MATTR_PRW);
	if (res)
		return res;

	res = vm_set_prot(&spc->uctx, sec_buf_addr, stmm_sec_buf_size,
			  TEE_MATTR_URW | TEE_MATTR_PRW);
	if (res)
		return res;

	DMSG("stmm load address %#"PRIxVA, image_addr);

	boot_info = (struct secure_partition_boot_info *)sec_buf_addr;
	mp_info = (struct secure_partition_mp_info *)(boot_info + 1);
	*boot_info = (struct secure_partition_boot_info){
		.h.type = SP_PARAM_SP_IMAGE_BOOT_INFO,
		.h.version = SP_PARAM_VERSION_1,
		.h.size = sizeof(struct secure_partition_boot_info),
		.h.attr = 0,
		.sp_mem_base = sp_addr,
		.sp_mem_limit = sp_addr + sp_size,
		.sp_image_base = image_addr,
		.sp_stack_base = stack_addr,
		.sp_heap_base = heap_addr,
		.sp_ns_comm_buf_base = comm_buf_addr,
		.sp_shared_buf_base = sec_buf_addr,
		.sp_image_size = stmm_image_size,
		.sp_pcpu_stack_size = stmm_stack_size,
		.sp_heap_size = stmm_heap_size,
		.sp_ns_comm_buf_size = stmm_ns_comm_buf_size,
		.sp_shared_buf_size = stmm_sec_buf_size,
		.num_sp_mem_regions = 6,
		.num_cpus = 1,
		.mp_info = mp_info,
	};
	mp_info->mpidr = read_mpidr_el1();
	mp_info->linear_id = 0;
	mp_info->flags = MP_INFO_FLAG_PRIMARY_CPU;
	spc->ns_comm_buf_addr = comm_buf_addr;
	spc->ns_comm_buf_size = stmm_ns_comm_buf_size;

	init_stmm_regs(spc, sec_buf_addr,
		       (vaddr_t)(mp_info + 1) - sec_buf_addr,
		       stack_addr + stmm_stack_size, image_addr);

	return sec_part_enter_user_mode(spc);
}

TEE_Result sec_part_init_session(const TEE_UUID *uuid,
				 struct tee_ta_session *sess)
{
	struct sec_part_ctx *spc = NULL;
	TEE_Result res = TEE_SUCCESS;

	if (memcmp(uuid, &stmm_uuid, sizeof(*uuid)))
		return TEE_ERROR_ITEM_NOT_FOUND;

	spc = sec_part_alloc_ctx(uuid);
	if (!spc)
		return TEE_ERROR_OUT_OF_MEMORY;

	spc->is_initializing = true;

	mutex_lock(&tee_ta_mutex);
	sess->ts_sess.ctx = &spc->ta_ctx.ts_ctx;
	mutex_unlock(&tee_ta_mutex);

	ts_push_current_session(&sess->ts_sess);
	res = load_stmm(spc);
	ts_pop_current_session();
	tee_mmu_set_ctx(NULL);
	if (res) {
		sess->ts_sess.ctx = NULL;
		spc->ta_ctx.ts_ctx.ops->destroy(&spc->ta_ctx.ts_ctx);

		return res;
	}

	mutex_lock(&tee_ta_mutex);
	spc->is_initializing = false;
	TAILQ_INSERT_TAIL(&tee_ctxes, &spc->ta_ctx, link);
	mutex_unlock(&tee_ta_mutex);

	return TEE_SUCCESS;
}

static TEE_Result stmm_enter_open_session(struct ts_session *s)
{
	struct sec_part_ctx *spc = to_sec_part_ctx(s->ctx);
	struct tee_ta_session *ta_sess = to_ta_session(s);
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE);

	if (ta_sess->param->types != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;

	if (spc->is_initializing) {
		/* StMM is initialized in sec_part_init_session() */
		ta_sess->err_origin = TEE_ORIGIN_TEE;
		return TEE_ERROR_BAD_STATE;
	}

	return TEE_SUCCESS;
}

static TEE_Result stmm_enter_invoke_cmd(struct ts_session *s, uint32_t cmd)
{
	struct sec_part_ctx *spc = to_sec_part_ctx(s->ctx);
	struct tee_ta_session *ta_sess = to_ta_session(s);
	TEE_Result res = TEE_SUCCESS;
	TEE_Result __maybe_unused tmp_res = TEE_SUCCESS;
	unsigned int ns_buf_size = 0;
	struct param_mem *mem = NULL;
	void *va = NULL;
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INOUT,
						TEE_PARAM_TYPE_VALUE_OUTPUT,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE);

	if (cmd != PTA_STMM_CMD_COMMUNICATE)
		return TEE_ERROR_BAD_PARAMETERS;

	if (ta_sess->param->types != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;

	mem = &ta_sess->param->u[0].mem;
	ns_buf_size = mem->size;
	if (ns_buf_size > spc->ns_comm_buf_size) {
		mem->size = spc->ns_comm_buf_size;
		return TEE_ERROR_EXCESS_DATA;
	}

	res = mobj_inc_map(mem->mobj);
	if (res)
		return res;

	va = mobj_get_va(mem->mobj, mem->offs);
	if (!va) {
		EMSG("Can't get a valid VA for NS buffer");
		res = TEE_ERROR_BAD_PARAMETERS;
		goto out_va;
	}

	spc->regs.x[0] = FFA_MSG_SEND_DIRECT_REQ_64;
	spc->regs.x[1] = (stmm_pta_id << 16) | stmm_id;
	spc->regs.x[2] = FFA_PARAM_MBZ;
	spc->regs.x[3] = spc->ns_comm_buf_addr;
	spc->regs.x[4] = ns_buf_size;
	spc->regs.x[5] = 0;
	spc->regs.x[6] = 0;
	spc->regs.x[7] = 0;

	ts_push_current_session(s);

	memcpy((void *)spc->ns_comm_buf_addr, va, ns_buf_size);

	res = sec_part_enter_user_mode(spc);
	if (res)
		goto out_session;
	/*
	 * Copy the SPM response from secure partition back to the non-secure
	 * buffer of the client that called us.
	 */
	ta_sess->param->u[1].val.a = spc->regs.x[4];

	memcpy(va, (void *)spc->ns_comm_buf_addr, ns_buf_size);

out_session:
	ts_pop_current_session();
out_va:
	tmp_res = mobj_dec_map(mem->mobj);
	assert(!tmp_res);

	return res;
}

static void stmm_enter_close_session(struct ts_session *s __unused)
{
}

static void sec_part_dump_state(struct ts_ctx *ctx)
{
	user_mode_ctx_print_mappings(to_user_mode_ctx(ctx));
}

static uint32_t sec_part_get_instance_id(struct ts_ctx *ctx)
{
	return to_sec_part_ctx(ctx)->uctx.vm_info.asid;
}

static void sec_part_ctx_destroy(struct ts_ctx *ctx)
{
	struct sec_part_ctx *spc = to_sec_part_ctx(ctx);

	tee_pager_rem_um_areas(&spc->uctx);
	vm_info_final(&spc->uctx);
	free(spc);
}

static uint32_t sp_svc_get_mem_attr(vaddr_t va)
{
	TEE_Result res = TEE_ERROR_BAD_PARAMETERS;
	struct ts_session *sess = NULL;
	struct sec_part_ctx *spc = NULL;
	uint16_t attrs = 0;
	uint16_t perm = 0;

	if (!va)
		goto err;

	sess = ts_get_current_session();
	spc = to_sec_part_ctx(sess->ctx);

	res = vm_get_prot(&spc->uctx, va, SMALL_PAGE_SIZE, &attrs);
	if (res)
		goto err;

	if (attrs & TEE_MATTR_UR)
		perm |= SP_MEM_ATTR_ACCESS_RO;
	else if (attrs & TEE_MATTR_UW)
		perm |= SP_MEM_ATTR_ACCESS_RW;

	if (attrs & TEE_MATTR_UX)
		perm |= SP_MEM_ATTR_EXEC;

	return perm;
err:
	return SP_RET_DENIED;
}

static int sp_svc_set_mem_attr(vaddr_t va, unsigned int nr_pages, uint32_t perm)
{
	TEE_Result res = TEE_ERROR_BAD_PARAMETERS;
	struct ts_session *sess = NULL;
	struct sec_part_ctx *spc = NULL;
	size_t sz = 0;
	uint32_t prot = 0;

	if (!va || !nr_pages || MUL_OVERFLOW(nr_pages, SMALL_PAGE_SIZE, &sz))
		return SP_RET_INVALID_PARAM;

	if (perm & ~SP_MEM_ATTR_ALL)
		return SP_RET_INVALID_PARAM;

	sess = ts_get_current_session();
	spc = to_sec_part_ctx(sess->ctx);

	if ((perm & SP_MEM_ATTR_ACCESS_MASK) == SP_MEM_ATTR_ACCESS_RO)
		prot |= TEE_MATTR_UR;
	else if ((perm & SP_MEM_ATTR_ACCESS_MASK) == SP_MEM_ATTR_ACCESS_RW)
		prot |= TEE_MATTR_URW;

	if ((perm & SP_MEM_ATTR_EXEC_NEVER) == SP_MEM_ATTR_EXEC)
		prot |= TEE_MATTR_UX;

	res = vm_set_prot(&spc->uctx, va, sz, prot);
	if (res)
		return SP_RET_DENIED;

	return SP_RET_SUCCESS;
}

static bool return_helper(bool panic, uint32_t panic_code,
			  struct thread_svc_regs *svc_regs)
{
	if (!panic) {
		struct ts_session *sess = ts_get_current_session();
		struct sec_part_ctx *spc = to_sec_part_ctx(sess->ctx);
		size_t n = 0;

		/* Save the return values from StMM */
		for (n = 0; n <= 7; n++)
			spc->regs.x[n] = *(&svc_regs->x0 + n);

		spc->regs.sp = svc_regs->sp_el0;
		spc->regs.pc = svc_regs->elr;
		spc->regs.cpsr = svc_regs->spsr;
	}

	svc_regs->x0 = 0;
	svc_regs->x1 = panic;
	svc_regs->x2 = panic_code;

	return false;
}

static void service_compose_direct_resp(struct thread_svc_regs *regs,
					uint32_t ret_val)
{
	uint16_t src_id = 0;
	uint16_t dst_id = 0;

	/* extract from request */
	src_id = (regs->x1 >> 16) & UINT16_MAX;
	dst_id = regs->x1 & UINT16_MAX;

	/* compose message */
	regs->x0 = FFA_MSG_SEND_DIRECT_RESP_64;
	/* swap endpoint ids */
	regs->x1 = SHIFT_U32(dst_id, 16) | src_id;
	regs->x2 = FFA_PARAM_MBZ;
	regs->x3 = ret_val;
	regs->x4 = 0;
	regs->x5 = 0;
	regs->x6 = 0;
	regs->x7 = 0;
}

/*
 * Combined read from secure partition, this will open, read and
 * close the file object.
 */
static TEE_Result sec_storage_obj_read(unsigned long storage_id, char *obj_id,
				       unsigned long obj_id_len, void *data,
				       unsigned long len, unsigned long offset,
				       unsigned long flags)
{
	const struct tee_file_operations *fops = NULL;
	TEE_Result res = TEE_ERROR_BAD_STATE;
	struct ts_session *sess = NULL;
	struct tee_file_handle *fh = NULL;
	struct sec_part_ctx *spc = NULL;
	struct tee_pobj *po = NULL;
	size_t file_size = 0;
	size_t read_len = 0;

	fops = tee_svc_storage_file_ops(storage_id);
	if (!fops)
		return TEE_ERROR_ITEM_NOT_FOUND;

	if (obj_id_len > TEE_OBJECT_ID_MAX_LEN)
		return TEE_ERROR_BAD_PARAMETERS;

	sess = ts_get_current_session();
	spc = to_sec_part_ctx(sess->ctx);
	res = tee_mmu_check_access_rights(&spc->uctx,
					  TEE_MEMORY_ACCESS_WRITE |
					  TEE_MEMORY_ACCESS_ANY_OWNER,
					  (uaddr_t)data, len);
	if (res != TEE_SUCCESS)
		return res;

	res = tee_pobj_get(&sess->ctx->uuid, obj_id, obj_id_len, flags,
			   false, fops, &po);
	if (res != TEE_SUCCESS)
		return res;

	res = po->fops->open(po, &file_size, &fh);
	if (res != TEE_SUCCESS)
		goto out;

	read_len = len;
	res = po->fops->read(fh, offset, data, &read_len);
	if (res == TEE_ERROR_CORRUPT_OBJECT) {
		EMSG("Object corrupt");
		po->fops->remove(po);
	} else if (res == TEE_SUCCESS && len != read_len) {
		res = TEE_ERROR_CORRUPT_OBJECT;
	}

	po->fops->close(&fh);

out:
	tee_pobj_release(po);

	return res;
}

/*
 * Combined write from secure partition, this will create/open, write and
 * close the file object.
 */
static TEE_Result sec_storage_obj_write(unsigned long storage_id, char *obj_id,
					unsigned long obj_id_len, void *data,
					unsigned long len, unsigned long offset,
					unsigned long flags)

{
	const struct tee_file_operations *fops = NULL;
	struct ts_session *sess = NULL;
	struct tee_file_handle *fh = NULL;
	struct sec_part_ctx *spc = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct tee_pobj *po = NULL;

	fops = tee_svc_storage_file_ops(storage_id);
	if (!fops)
		return TEE_ERROR_ITEM_NOT_FOUND;

	if (obj_id_len > TEE_OBJECT_ID_MAX_LEN)
		return TEE_ERROR_BAD_PARAMETERS;

	sess = ts_get_current_session();
	spc = to_sec_part_ctx(sess->ctx);
	res = tee_mmu_check_access_rights(&spc->uctx,
					  TEE_MEMORY_ACCESS_READ |
					  TEE_MEMORY_ACCESS_ANY_OWNER,
					  (uaddr_t)data, len);
	if (res != TEE_SUCCESS)
		return res;

	res = tee_pobj_get(&sess->ctx->uuid, obj_id, obj_id_len, flags,
			   false, fops, &po);
	if (res != TEE_SUCCESS)
		return res;

	res = po->fops->open(po, NULL, &fh);
	if (res == TEE_ERROR_ITEM_NOT_FOUND)
		res = po->fops->create(po, false, NULL, 0, NULL, 0, NULL, 0,
				       &fh);
	if (res == TEE_SUCCESS) {
		res = po->fops->write(fh, offset, data, len);
		po->fops->close(&fh);
	}

	tee_pobj_release(po);

	return res;
}

static bool stmm_handle_mem_mgr_service(struct thread_svc_regs *regs)
{
	uint32_t action = regs->x3;
	uintptr_t va = regs->x4;
	uint32_t nr_pages = regs->x5;
	uint32_t perm = regs->x6;

	switch (action) {
	case FFA_SVC_MEMORY_ATTRIBUTES_GET_64:
		service_compose_direct_resp(regs, sp_svc_get_mem_attr(va));
		return true;
	case FFA_SVC_MEMORY_ATTRIBUTES_SET_64:
		service_compose_direct_resp(regs,
					    sp_svc_set_mem_attr(va, nr_pages,
								perm));
		return true;
	default:
		EMSG("Undefined service id %#"PRIx32, action);
		service_compose_direct_resp(regs, SP_RET_INVALID_PARAM);
		return true;
	}
}

#define FILENAME "EFI_VARS"
static bool stmm_handle_storage_service(struct thread_svc_regs *regs)
{
	uint32_t flags = TEE_DATA_FLAG_ACCESS_READ |
			 TEE_DATA_FLAG_ACCESS_WRITE |
			 TEE_DATA_FLAG_SHARE_READ |
			 TEE_DATA_FLAG_SHARE_WRITE;
	uint32_t action = regs->x3;
	void *va = (void *)regs->x4;
	unsigned long len = regs->x5;
	unsigned long offset = regs->x6;
	char obj_id[] = FILENAME;
	size_t obj_id_len = strlen(obj_id);
	TEE_Result res = TEE_SUCCESS;

	switch (action) {
	case FFA_SVC_RPMB_READ:
		res = sec_storage_obj_read(TEE_STORAGE_PRIVATE_RPMB, obj_id,
					   obj_id_len, va, len, offset, flags);
		service_compose_direct_resp(regs, res);

		return true;
	case FFA_SVC_RPMB_WRITE:
		res = sec_storage_obj_write(TEE_STORAGE_PRIVATE_RPMB, obj_id,
					    obj_id_len, va, len, offset, flags);
		service_compose_direct_resp(regs, res);

		return true;
	default:
		EMSG("Undefined service id %#"PRIx32, action);
		service_compose_direct_resp(regs, SP_RET_INVALID_PARAM);
		return true;
	}
}

static bool spm_eret_error(int32_t error_code, struct thread_svc_regs *regs)
{
	regs->x0 = FFA_ERROR;
	regs->x1 = FFA_PARAM_MBZ;
	regs->x2 = error_code;
	regs->x3 = FFA_PARAM_MBZ;
	regs->x4 = FFA_PARAM_MBZ;
	regs->x5 = FFA_PARAM_MBZ;
	regs->x6 = FFA_PARAM_MBZ;
	regs->x7 = FFA_PARAM_MBZ;
	return true;
}

static bool spm_handle_direct_req(struct thread_svc_regs *regs)
{
	uint16_t dst_id = regs->x1 & UINT16_MAX;

	/* Look-up of destination endpoint */
	if (dst_id == mem_mgr_id)
		return stmm_handle_mem_mgr_service(regs);
	else if (dst_id == ffa_storage_id)
		return stmm_handle_storage_service(regs);

	EMSG("Undefined endpoint id %#"PRIx16, dst_id);
	return spm_eret_error(SP_RET_INVALID_PARAM, regs);
}

static bool spm_handle_svc(struct thread_svc_regs *regs)
{
	switch (regs->x0) {
	case FFA_VERSION:
		DMSG("Received FFA version");
		regs->x0 = MAKE_FFA_VERSION(FFA_VERSION_MAJOR,
					    FFA_VERSION_MINOR);
		return true;
	case FFA_MSG_SEND_DIRECT_RESP_64:
		DMSG("Received FFA direct response");
		return return_helper(false, 0, regs);
	case FFA_MSG_SEND_DIRECT_REQ_64:
		DMSG("Received FFA direct request");
		return spm_handle_direct_req(regs);
	default:
		EMSG("Undefined syscall %#"PRIx32, (uint32_t)regs->x0);
		return return_helper(true /*panic*/, 0xabcd, regs);
	}
}

const struct ts_ops secure_partition_ops __rodata_unpaged = {
	.enter_open_session = stmm_enter_open_session,
	.enter_invoke_cmd = stmm_enter_invoke_cmd,
	.enter_close_session = stmm_enter_close_session,
	.dump_state = sec_part_dump_state,
	.destroy = sec_part_ctx_destroy,
	.get_instance_id = sec_part_get_instance_id,
	.handle_svc = spm_handle_svc,
};
