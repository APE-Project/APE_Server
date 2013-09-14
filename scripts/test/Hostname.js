Ape.log(' =====================================>>> \n Start up for test/Hostname.js\n');
try {
//	Ape.log(' =====================================>>> \n Starting test Ape.getHostByName\n');
//	var r = Ape.getHostByName('www.verpeteren.nl');
//	Ape.log('returned: ' + r);
	Ape.log(' =====================================>>> \n Starting test Ape.os.getHostByName\n');
	var r = os.getHostByName('www.verpeteren.nl');
	Ape.log('returned: ' + r);
	Ape.log(' =====================================>>> \n Starting test Ape.os.resolveHostByName\n');
	var r = os.resolveHostByName('www.verpeteren.nl', function(ip) {
		Ape.log('resolved: ' + ip);
	});
	Ape.log(' =====================================>>> \n Starting test fake Ape.os.resolveHostByName\n');
	var r = os.resolveHostByName('www.this.should.not.work.@all', function(ip) {
		Ape.log('resolved: ' + ip);
	});

} catch (e) {
	Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
}
Ape.log('<<< =====================================\n');
