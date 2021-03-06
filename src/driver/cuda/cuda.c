/*
 *  Multi2Sim
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <arch/fermi/emu/emu.h>
#include <arch/fermi/emu/grid.h>
#include <arch/x86/emu/context.h>
#include <arch/x86/emu/emu.h>
#include <arch/x86/emu/regs.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/list.h>
#include <mem-system/memory.h>

#include "cuda.h"
#include "function.h"
#include "function-arg.h"
#include "module.h"


/*
 * Global Variables
 */

/* Debug */
int cuda_debug_category = 1;

/* Error messages */
char *cuda_err_code =
	"\tAn invalid function code was generated by your application in a CUDA system\n"
	"\tcall. Probably, this means that your application is using an incompatible\n"
	"\tversion of the Multi2Sim CUDA runtime/driver library ('libm2s-cuda'). Please\n"
	"\trecompile your application and try again.\n";

/* List of CUDA driver call names */
char *cuda_call_name[cuda_call_count + 1] =
{
	NULL,
#define CUDA_DEFINE_CALL(name) #name,
#include "cuda.dat"
#undef CUDA_DEFINE_CALL
	NULL
};

/* List of CUDA driver functions */
cuda_func_t cuda_func_table[cuda_call_count + 1] =
{
	NULL,
#define CUDA_DEFINE_CALL(name) cuda_func_##name,
#include "cuda.dat"
#undef CUDA_DEFINE_CALL
	NULL
};




/*
 * CUDA Global Functions
 */

int cuda_abi_call(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	int code;
	int ret;

	/* Function code */
	code = regs->ebx;
	if (code <= cuda_call_invalid || code >= cuda_call_count)
		fatal("%s: invalid CUDA function (code %d).\n%s",
			__FUNCTION__, code, cuda_err_code);

	/* Debug */
	cuda_debug("CUDA call '%s' (code %d)\n", cuda_call_name[code], code);

	/* Call */
	assert(cuda_func_table[code]);
	ret = cuda_func_table[code](ctx);

	return ret;
}

void cuda_init(void)
{
}

void cuda_done(void)
{
	int i;
	struct cuda_module_t *module;
	struct cuda_function_t *function;

	if (!module_list)
		return;

	LIST_FOR_EACH(module_list, i)
	{
		module = list_get(module_list, i);
		if (module)
			cuda_module_free(module);
	}
	list_free(module_list);

	if (!function_list)
		return;

	LIST_FOR_EACH(function_list, i)
	{
		function = list_get(function_list, i);
		if (function)
			cuda_function_free(function);
	}
	list_free(function_list);
}




/*
 * CUDA call - versionCheck
 *
 * @param struct cuda_version_t *version;
 *	Structure where the version of the CUDA driver implementation will be
 *	dumped. To succeed, the major version should match in the driver
 *	library (guest) and driver implementation (host), whereas the minor
 *	version should be equal or higher in the implementation (host).
 *
 *	Features should be added to the CUDA driver (guest and host) using the
 *	following rules:
 *	1)  If the guest library requires a new feature from the host
 *	    implementation, the feature is added to the host, and the minor
 *	    version is updated to the current Multi2Sim SVN revision both in
 *	    host and guest.
 *          All previous services provided by the host should remain available
 *          and backward-compatible. Executing a newer library on the older
 *          simulator will fail, but an older library on the newer simulator
 *          will succeed.
 *      2)  If a new feature is added that affects older services of the host
 *          implementation breaking backward compatibility, the major version is
 *          increased by 1 in the host and guest code.
 *          Executing a library with a different (lower or higher) major version
 *          than the host implementation will fail.
 *
 * @return
 *	The runtime implementation version is return in argument 'version'.
 *	The return value is always 0.
 */

int cuda_func_versionCheck(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;
	struct cuda_version_t version;

	version.major = CUDA_VERSION_MAJOR;
	version.minor = CUDA_VERSION_MINOR;

	cuda_debug("\tout: version.major=%d\n", version.major);
	cuda_debug("\tout: version.minor=%d\n", version.minor);

	mem_write(mem, regs->ecx, sizeof version, &version);

	return 0;
}




/*
 * CUDA call - cuInit
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuInit(X86Context *ctx)
{
	/* Create module list*/
	module_list = list_create();

	/* Create function list*/
	function_list = list_create();

	return 0;
}




/*
 * CUDA call - cuDeviceTotalMem
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuDeviceTotalMem(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int total_global_mem_size;

	total_global_mem_size = regs->ecx;

	cuda_debug("\tout: total=%u\n", frm_emu->total_global_mem_size);

	mem_write(mem, total_global_mem_size, sizeof(unsigned int), &(frm_emu->total_global_mem_size));

	return 0;
}




/*
 * CUDA call - cuModuleLoad
 *
 * @param const char *fname;
 *      Filename of module to load.
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuModuleLoad(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	struct cuda_module_t *module;
	char cubin_path[MAX_STRING_SIZE] = "";

	/* Get kernel binary */
	if (regs->ecx != 0)
		mem_read(mem, regs->ecx, MAX_STRING_SIZE, cubin_path);
	
	/* Create module */
	module = cuda_module_create(cubin_path);

	cuda_debug("\tout: module.id=0x%08x\n", module->id);

	return 0;
}




/*
 * CUDA call - cuModuleUnload
 *
 * @param unsign int module_id;
 *      ID of the module to unload.
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuModuleUnload(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	struct cuda_module_t *module;
	unsigned int module_id;

	/* Get module */
	module_id = regs->ecx;
	module = (struct cuda_module_t *)list_get(module_list, module_id);

	/* Free module */
	cuda_module_free(module);

	return 0;
}




/*
 * CUDA call - cuModuleGetFunction
 *
 * @param unsinged int module_id;
 *      ID of the module to retrieve function from.
 *
 * @param char *function_name;
 *      Name of function to retrieve.
 *
 * @param unsigned long long int *inst_buffer;
 *      Instruction binary of function.
 *
 * @param unsigned int inst_buffer_size;
 *      Size of instruction binary.
 *
 * @param unsigned int num_gpr_used;
 *      Number of GPRs used per thread.
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuModuleGetFunction(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int module_id;
	char function_name[MAX_STRING_SIZE];
	unsigned long long int *inst_buffer;
	unsigned int inst_buffer_size;
	unsigned int num_gpr_used;

	struct cuda_module_t *module;
	struct cuda_function_t *function;

	module_id = regs->ecx;
	mem_read(mem, regs->edx, MAX_STRING_SIZE, function_name);
	inst_buffer_size = regs->edi;
	inst_buffer = (unsigned long long int *)xcalloc(1, inst_buffer_size);
	mem_read(mem, regs->esi, inst_buffer_size, inst_buffer);
	num_gpr_used = regs->ebp;

	cuda_debug("\tin: module.id = 0x%08x\n", module_id);
	cuda_debug("\tin: function_name = %s\n", function_name);
	cuda_debug("\tin: inst_buffer = %p\n", inst_buffer);
	cuda_debug("\tin: inst_buffer_size = %u\n", inst_buffer_size);
	cuda_debug("\tin: num_gpr_used = %u\n", num_gpr_used);

	/* Get module */
	module = (struct cuda_module_t *)list_get(module_list, module_id);

	/* Create function */
	function = cuda_function_create(module, function_name, inst_buffer,
			inst_buffer_size, num_gpr_used);

	/* Free */
	free(inst_buffer);

	cuda_debug("\tout: function.id=0x%08x\n", function->id);

	return 0;
}




/*
 * CUDA call - cuMemGetInfo
 *
 * @param size_t *free;
 *      Returned free memory in bytes
 *
 * @param size_t *total;
 *      Returned total memory in bytes
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuMemGetInfo(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int free;
	unsigned int total;

	free = regs->ecx;
	total = regs->edx;

	cuda_debug("\tout: free=%u\n", frm_emu->free_global_mem_size);
	cuda_debug("\tout: total=%u\n", frm_emu->total_global_mem_size);

	mem_write(mem, free, sizeof(unsigned int), &(frm_emu->free_global_mem_size));
	mem_write(mem, total, sizeof(unsigned int), &(frm_emu->total_global_mem_size));

	return 0;
}




/*
 * CUDA call - cuMemAlloc
 *
 * @param CUdeviceptr *dptr;
 *      Returned device pointer.
 *
 * @param size_t bytesize;
 *      Requested allocation size in bytes.
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuMemAlloc(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int dptr;
	unsigned int bytesize;

	dptr = regs->ecx;
	bytesize = regs->edx;

	cuda_debug("\tin: bytesize=%u\n", bytesize);

	/* Assign position in device global memory */
	frm_emu->global_mem_top += bytesize;
	frm_emu->free_global_mem_size -= bytesize;

	cuda_debug("\tout: dptr=0x%08x\n", frm_emu->global_mem_top);

	mem_write(mem, dptr, sizeof(unsigned int), &(frm_emu->global_mem_top));

	return 0;
}




/*
 * CUDA call - cuMemFree
 *
 * @param CUdeviceptr dptr;
 *      Pointer to memory to free.
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuMemFree(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	unsigned int dptr;

	dptr = regs->ecx;

	cuda_debug("\tin: dptr=0x%08x\n", dptr);

	return 0;
}




/*
 * CUDA call - cuMemcpyHtoD
 *
 * @param CUdeviceptr dstDevice;
 *      Destination device pointer.
 *
 * @param const void *srcHost;
 *      Source host pointer.
 *
 * @param size_t ByteCount;
 *      Size of memory copy in bytes.
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuMemcpyHtoD(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int dstDevice;
	unsigned int srcHost;
	unsigned int ByteCount;
	void *buf;

	dstDevice = regs->ecx;
	srcHost = regs->edx;
	ByteCount = regs->esi;

	cuda_debug("\tin: dstDevice=0x%08x\n", dstDevice);
	cuda_debug("\tin: srcHost=0x%08x\n", srcHost);
	cuda_debug("\tin: ByteCount=%u\n", ByteCount);

	/* Copy */
	buf = xmalloc(ByteCount);
	mem_read(mem, srcHost, ByteCount, buf);
	mem_write(frm_emu->global_mem, dstDevice, ByteCount, buf);
	free(buf);

	return 0;
}




/*
 * CUDA call - cuMemcpyDtoH
 *
 * @param void *dstHost;
 *      Destination host pointer.
 *
 * @param CUdeviceptr srcDevice;
 *      Source device pointer.
 *
 * @param size_t ByteCount;
 *      Size of memory copy in bytes.
 *
 * @return
 *	The return value is always 0 on success.
 */

int cuda_func_cuMemcpyDtoH(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int dstHost;
	unsigned int srcDevice;
	unsigned int ByteCount;
	void *buf;

	dstHost = regs->ecx;
	srcDevice = regs->edx;
	ByteCount = regs->esi;

	cuda_debug("\tin: dstHost=0x%08x\n", dstHost);
	cuda_debug("\tin: srcDevice=0x%08x\n", srcDevice);
	cuda_debug("\tin: ByteCount=%u\n", ByteCount);

	/* Copy */
	buf = xmalloc(ByteCount);
	mem_read(frm_emu->global_mem, srcDevice, ByteCount, buf);
	mem_write(mem, dstHost, ByteCount, buf);
	free(buf);

	return 0;
}




/*
 * CUDA call - cuLaunchKernel
 *
 * @param CUfunction f;
 *      Kernel to launch.
 *
 * @param unsigned int gridDimX;
 *      Width of grid in blocks.
 *
 * @param unsigned int gridDimY;
 *      Height of grid in blocks.
 *
 * @param unsigned int gridDimZ;
 *      Depth of grid in blocks.
 *
 * @param unsigned int blockDimX;
 *      X dimension of each thread block.
 *
 * @param unsigned int blockDimY;
 *      Y dimension of each thread block.
 *
 * @param unsigned int blockDimZ;
 *      Z dimension of each thread block.
 *
 * @param unsigned int sharedMemBytes;
 *      Dynamic shared-memory size per thread block in bytes.
 *
 * @param CUstream hStream;
 *      Stream identifier.
 *
 * @param void **kernelParams;
 *      Array of pointers to kernel parameters.
 *
 * @param void **extra;
 *      Extra options.
 *
 * @return
 *	The return value is always 0 on success.
 */

void frm_grid_set_free_notify_func(struct frm_grid_t *grid,
	void (*func)(void *), void *user_data)
{
	grid->free_notify_func = func;
	grid->free_notify_data = user_data;
}

static void cuda_abi_frm_kernel_launch_finish(void *user_data)
{
	struct cuda_abi_frm_kernel_launch_info_t *info = user_data;
	struct cuda_function_t *kernel = info->function;
	struct frm_grid_t *grid = info->grid;

	/* Debug */
	cuda_debug("Grid %d running kernel '%s' finished\n",
			grid->id, kernel->name);

	/* Set 'finished' flag in launch info */
	info->finished = 1;

	/* Force the x86 emulator to check which suspended contexts can wakeup,
	 * based on their new state. */
	X86EmuProcessEventsSchedule(x86_emu);
}

static int cuda_abi_frm_kernel_launch_can_wakeup(X86Context *ctx,
		void *user_data)
{
	struct cuda_abi_frm_kernel_launch_info_t *info = user_data;

	/* NOTE: the grid has been freed at this point if it finished
	 * execution, so field 'info->grid' should not be accessed. We
	 * use flag 'info->finished' instead. */
	return info->finished;
}

static void cuda_abi_frm_kernel_launch_wakeup(X86Context *ctx,
		void *user_data)
{
	struct cuda_abi_frm_kernel_launch_info_t *info = user_data;

	/* Free info object */
	free(info);
}

int cuda_func_cuLaunchKernel(X86Context *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int args[11];
	unsigned int function_id;
	unsigned int gridDim[3];
	unsigned int blockDim[3];
	unsigned int sharedMemBytes;
	unsigned int hStream;
	unsigned int kernelParams;
	unsigned int extra;

	struct cuda_function_t *function;
	struct cuda_function_arg_t *arg;
	char arg_name[MAX_STRING_SIZE];
	unsigned int arg_ptr;
	unsigned int arg_value;
	struct frm_grid_t *grid;
	int i;
	struct cuda_abi_frm_kernel_launch_info_t *info;

	/* Read arguments */
	mem_read(mem, regs->ecx, 11 * sizeof(unsigned int), args);
	function_id = args[0];
	gridDim[0] = args[1];
	gridDim[1] = args[2];
	gridDim[2] = args[3];
	blockDim[0] = args[4];
	blockDim[1] = args[5];
	blockDim[2] = args[6];
	sharedMemBytes = args[7];
	hStream = args[8];
	kernelParams = args[9];
	extra = args[10];

	/* Debug */
	cuda_debug("\tfunction_id = 0x%08x\n", function_id);
	cuda_debug("\tgridDimX = %u\n", gridDim[0]);
	cuda_debug("\tgridDimY = %u\n", gridDim[1]);
	cuda_debug("\tgridDimZ = %u\n", gridDim[2]);
	cuda_debug("\tblockDimX = %u\n", blockDim[0]);
	cuda_debug("\tblockDimY = %u\n", blockDim[1]);
	cuda_debug("\tblockDimZ = %u\n", blockDim[2]);
	cuda_debug("\tsharedMemBytes = %u\n", sharedMemBytes);
	cuda_debug("\thStream = 0x%08x\n", hStream);
	cuda_debug("\tkernelParams = 0x%08x\n", kernelParams);
	cuda_debug("\textra = %u\n", extra);

	/* Get function */
	function = list_get(function_list, function_id);

	/* Arguments */
	for (i = 0; i < sizeof(kernelParams); ++i)
	{
		/* Get argument value */
		mem_read(mem, kernelParams + i * 4, sizeof(unsigned int), 
				&arg_ptr);
		mem_read(mem, arg_ptr, sizeof(unsigned int), &arg_value);

		/* Create argument */
		snprintf(arg_name, MAX_STRING_SIZE, "arg_%d", i);
		arg = cuda_function_arg_create(arg_name);

		/* Initialize argument */
		arg->kind = CUDA_FUNCTION_ARG_KIND_POINTER;
		arg->mem_scope = CUDA_MEM_SCOPE_GLOBAL;
		arg->access_type = CUDA_FUNCTION_ARG_READ_WRITE;
		arg->value = arg_value;

		/* Add to list */
		list_add(function->arg_list, arg);
	}

	/* Create grid */
	grid = frm_grid_create(function);

	/* Set up grid */
	frm_grid_setup_size(grid, gridDim, blockDim);
	frm_grid_setup_const_mem(grid);
	frm_grid_setup_args(grid);

	/* Add to pending list */
	list_add(frm_emu->pending_grids, grid);

	/* Set up call-back function to be run when grid finishes */
	info = xcalloc(1, sizeof(struct cuda_abi_frm_kernel_launch_info_t));
	info->function= function;
	info->grid = grid;
	frm_grid_set_free_notify_func(grid, cuda_abi_frm_kernel_launch_finish, info);

	/* Suspend x86 context until grid finishes */
	X86ContextSuspend(ctx, cuda_abi_frm_kernel_launch_can_wakeup, info,
			cuda_abi_frm_kernel_launch_wakeup, info);

	return 0;
}

