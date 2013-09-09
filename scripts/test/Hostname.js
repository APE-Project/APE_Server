Ape.log(' =====================================>>> \n Start up for test/Hostname.js\n');
try {
	var r = os.getHostByName('www.verpeteren.nl');
	Ape.log('returned: ' + r);
} catch (e) {
	Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
}
Ape.log('<<< =====================================\n');
