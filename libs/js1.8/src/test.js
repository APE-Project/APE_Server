//	jit(true);
	 function addTo(a, n) {
	   for (var i = 0; i < n; ++i)
	     a = a + i;
	   return a;
	 }

	 var t0 = new Date();
	 var n = addTo(0, 10000000);
	 print(n);
	 print(new Date() - t0);


         var t0 = new Date();
         var n = addTo(0, 20000000);
         print(n);
         print(new Date() - t0);

