/* Area:	closure_call
   Purpose:	Check return value schar.
   Limitations:	none.
   PR:		none.
   Originator:	<andreast@gcc.gnu.org> 20031108	 */



/* { dg-do run } */
#include "ffitest.h"

static void cls_ret_schar_fn(ffi_cif* cif __UNUSED__, void* resp, void** args,
			     void* userdata __UNUSED__)
{
  *(ffi_arg*)resp = *(signed char *)args[0];
  printf("%d: %d\n",*(signed char *)args[0],
	 (int)*(ffi_arg *)(resp));
}
typedef signed char (*cls_ret_schar)(signed char);

int main (void)
{
  ffi_cif cif;
#ifndef USING_MMAP
  static ffi_closure cl;
#endif
  ffi_closure *pcl;
  ffi_type * cl_arg_types[2];
  signed char res;

#ifdef USING_MMAP
  pcl = allocate_mmap (sizeof(ffi_closure));
#else
  pcl = &cl;
#endif

  cl_arg_types[0] = &ffi_type_schar;
  cl_arg_types[1] = NULL;

  /* Initialize the cif */
  CHECK(ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1,
		     &ffi_type_schar, cl_arg_types) == FFI_OK);

  CHECK(ffi_prep_closure(pcl, &cif, cls_ret_schar_fn, NULL)  == FFI_OK);

  res = (*((cls_ret_schar)pcl))(127);
  /* { dg-output "127: 127" } */
  printf("res: %d\n", res);
  /* { dg-output "\nres: 127" } */

  exit(0);
}
