/* Area:		ffi_call, closure_call
   Purpose:		Check pointer arguments.
   Limitations:	none.
   PR:			none.
   Originator:	Blake Chaffin 6/6/2007	*/

/* { dg-do run { xfail mips*-*-* arm*-*-* strongarm*-*-* xscale*-*-* } } */
#include "ffitest.h"

void* cls_pointer_fn(void* a1, void* a2)
{
	void*	result	= (void*)((long)a1 + (long)a2);

	printf("0x%08x 0x%08x: 0x%08x\n", 
	       (unsigned int) a1, (unsigned int) a2, (unsigned int) result);

	return result;
}

static void
cls_pointer_gn(ffi_cif* cif __UNUSED__, void* resp, 
	       void** args, void* userdata __UNUSED__)
{
	void*	a1	= *(void**)(args[0]);
	void*	a2	= *(void**)(args[1]);

	*(void**)resp = cls_pointer_fn(a1, a2);
}

int main (void)
{
	ffi_cif	cif;
#ifndef USING_MMAP
	static ffi_closure	cl;
#endif
	ffi_closure*	pcl;
	void*			args[3];
//	ffi_type		cls_pointer_type;
	ffi_type*		arg_types[3];

#ifdef USING_MMAP
	pcl = allocate_mmap(sizeof(ffi_closure));
#else
	pcl = &cl;
#endif

/*	cls_pointer_type.size = sizeof(void*);
	cls_pointer_type.alignment = 0;
	cls_pointer_type.type = FFI_TYPE_POINTER;
	cls_pointer_type.elements = NULL;*/

	void*	arg1	= (void*)0x12345678;
	void*	arg2	= (void*)0x89abcdef;
	ffi_arg	res		= 0;

	arg_types[0] = &ffi_type_pointer;
	arg_types[1] = &ffi_type_pointer;
	arg_types[2] = NULL;

	CHECK(ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_pointer,
		arg_types) == FFI_OK);

	args[0] = &arg1;
	args[1] = &arg2;
	args[2] = NULL;

	ffi_call(&cif, FFI_FN(cls_pointer_fn), &res, args);
	/* { dg-output "0x12345678 0x89abcdef: 0x9be02467" } */
	printf("res: 0x%08x\n", (unsigned int) res);
	/* { dg-output "\nres: 0x9be02467" } */

	CHECK(ffi_prep_closure(pcl, &cif, cls_pointer_gn, NULL) == FFI_OK);

	res = (ffi_arg)((void*(*)(void*, void*))(pcl))(arg1, arg2);
	/* { dg-output "\n0x12345678 0x89abcdef: 0x9be02467" } */
	printf("res: 0x%08x\n", (unsigned int) res);
	/* { dg-output "\nres: 0x9be02467" } */

	exit(0);
}
