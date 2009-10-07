/* Area:		ffi_call, closure_call
   Purpose:		Check pointer arguments across multiple hideous stack frames.
   Limitations:	none.
   PR:			none.
   Originator:	Blake Chaffin 6/7/2007	*/

/* { dg-do run { xfail mips*-*-* arm*-*-* strongarm*-*-* xscale*-*-* } } */
#include "ffitest.h"

static	long dummyVar;

long dummy_func(
	long double a1, char b1,
	long double a2, char b2,
	long double a3, char b3,
	long double a4, char b4)
{
	return a1 + b1 + a2 + b2 + a3 + b3 + a4 + b4;
}

void* cls_pointer_fn2(void* a1, void* a2)
{
	long double	trample1	= (long)a1 + (long)a2;
	char		trample2	= ((char*)&a1)[0] + ((char*)&a2)[0];
	long double	trample3	= (long)trample1 + (long)a1;
	char		trample4	= trample2 + ((char*)&a1)[1];
	long double	trample5	= (long)trample3 + (long)a2;
	char		trample6	= trample4 + ((char*)&a2)[1];
	long double	trample7	= (long)trample5 + (long)trample1;
	char		trample8	= trample6 + trample2;

	dummyVar	= dummy_func(trample1, trample2, trample3, trample4,
		trample5, trample6, trample7, trample8);

	void*	result	= (void*)((long)a1 + (long)a2);

	printf("0x%08x 0x%08x: 0x%08x\n", 
	       (unsigned int) a1, (unsigned int) a2, (unsigned int) result);

	return result;
}

void* cls_pointer_fn1(void* a1, void* a2)
{
	long double	trample1	= (long)a1 + (long)a2;
	char		trample2	= ((char*)&a1)[0] + ((char*)&a2)[0];
	long double	trample3	= (long)trample1 + (long)a1;
	char		trample4	= trample2 + ((char*)&a1)[1];
	long double	trample5	= (long)trample3 + (long)a2;
	char		trample6	= trample4 + ((char*)&a2)[1];
	long double	trample7	= (long)trample5 + (long)trample1;
	char		trample8	= trample6 + trample2;

	dummyVar	= dummy_func(trample1, trample2, trample3, trample4,
		trample5, trample6, trample7, trample8);

	void*	result	= (void*)((long)a1 + (long)a2);

	printf("0x%08x 0x%08x: 0x%08x\n", a1, a2, result);

	result	= cls_pointer_fn2(result, a1);

	return result;
}

static void
cls_pointer_gn(ffi_cif* cif __UNUSED__, void* resp, 
	       void** args, void* userdata __UNUSED__)
{
	void*	a1	= *(void**)(args[0]);
	void*	a2	= *(void**)(args[1]);

	long double	trample1	= (long)a1 + (long)a2;
	char		trample2	= ((char*)&a1)[0] + ((char*)&a2)[0];
	long double	trample3	= (long)trample1 + (long)a1;
	char		trample4	= trample2 + ((char*)&a1)[1];
	long double	trample5	= (long)trample3 + (long)a2;
	char		trample6	= trample4 + ((char*)&a2)[1];
	long double	trample7	= (long)trample5 + (long)trample1;
	char		trample8	= trample6 + trample2;

	dummyVar	= dummy_func(trample1, trample2, trample3, trample4,
		trample5, trample6, trample7, trample8);

	*(void**)resp = cls_pointer_fn1(a1, a2);
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

	void*	arg1	= (void*)0x01234567;
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

	printf("\n");
	ffi_call(&cif, FFI_FN(cls_pointer_fn1), &res, args);

	printf("res: 0x%08x\n", (unsigned int) res);
	// { dg-output "\n0x01234567 0x89abcdef: 0x8acf1356" }
	// { dg-output "\n0x8acf1356 0x01234567: 0x8bf258bd" }
	// { dg-output "\nres: 0x8bf258bd" }

	CHECK(ffi_prep_closure(pcl, &cif, cls_pointer_gn, NULL) == FFI_OK);

	res = (ffi_arg)((void*(*)(void*, void*))(pcl))(arg1, arg2);

	printf("res: 0x%08x\n", (unsigned int) res);
	// { dg-output "\n0x01234567 0x89abcdef: 0x8acf1356" }
	// { dg-output "\n0x8acf1356 0x01234567: 0x8bf258bd" }
	// { dg-output "\nres: 0x8bf258bd" }

	exit(0);
}
