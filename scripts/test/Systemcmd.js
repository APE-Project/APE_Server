try {
	var r = Ape.system("/usr/bin/wget", "http://www.verpeteren.nl -o /tmp/www.verpeteren.nl.html");
	Ape.log('returned: ' + r);	
} catch (e) {
	Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
}
;