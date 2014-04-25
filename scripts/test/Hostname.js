Ape.log(' =====================================');
Ape.log('     Start up for test/Hostname.js    ');
Ape.log(' =====================================');
try {
	
	//FIRST TEST
	//(This function is blocking)
	Ape.log('\n >>> Starting test os.getHostByName');
	var r = os.getHostByName('www.verpeteren.nl');
	Ape.log('     returned: ' + r);
	
	//SECOND TEST
	Ape.log('\n >>> Starting test os.resolveHostByName');
	var r = os.resolveHostByName('www.verpeteren.nl', function(ip) {
		if (! ip) {
	 		Ape.log('os.resolveHostByName (2): Could not resolve host' );
	 	} else {
	 		Ape.log('os.resolveHostByName (2): ' + ip);
	 	}
	});
	Ape.log('     [Callback function will display "os.resolveHostByName (2)" when ready]\n');
	
	//THRIRD TEST
	Ape.log('\n >>> Starting test fake os.resolveHostByName');
	var r = os.resolveHostByName('www.this.should.not.work.@all', function(ip) {
		if (! ip) {
	 		Ape.log('os.resolveHostByName (3): Could not resolve host' );
	 	} else {
	 		Ape.log('os.resolveHostByName (3): ' + ip);
	 	}
	});
	Ape.log('     [Callback function will display "os.resolveHostByName (3)" when ready]\n');
	
	//FOURTH TEST
	Ape.log('\n >>> Starting test ip');
	var r = os.resolveHostByName('8.8.8.8', function(ip) {
		Ape.log('     os.resolveHostByName (4): ' + ip);
	});

} catch (e) {
	Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
}
Ape.log('\n\n');
