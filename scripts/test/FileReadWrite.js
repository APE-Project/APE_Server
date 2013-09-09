Ape.log(' =====================================>>> \n Start up for test/FileReadWrite.js\n');
try {
    	var fn = '/tmp/bla.txt';
    	os.system('/bin/rm', '-rf ' + fn);
	var rot13 =	'Guvf vf n yvggyr fgbel nobhg sbhe crbcyr anzrq Rirelobql, Fbzrobql, Nalobql, naq Abobql.' + '\n'+
            	'Gurer jnf na vzcbegnag wbo gb or qbar naq Rirelobql jnf fher gung Fbzrobql jbhyq qb vg.' + '\n'+
            	'Nalobql pbhyq unir qbar vg, ohg Abobql qvq vg.' + '\n'+
            	'Fbzrobql tbg natel nobhg gung orpnhfr vg jnf Rirelobql\'f wbo.' + '\n'+
            	'Rirelobql gubhtug gung Nalobql pbhyq qb vg, ohg Abobql ernyvmrq gung Rirelobql jbhyqa\'g qb vg.' + '\n'+
            	'Vg raqrq hc gung Rirelobql oynzrq Fbzrobql jura Abobql qvq jung Nalobql pbhyq unir qbar' +'\n'; 
	/*
	 * Append to a normal file
	 * */
	var r = os.writefile(fn, rot13, false);
	r = os.writefile(fn, rot13, true);
	if(r === true){
	    Ape.log('write succesfully: ' + r);
	}else{
	    Ape.log('Could not write file correctly: ' + r);
	}
	var twice = rot13 + rot13;
	var content = os.readfile(fn);
	if (content === twice) {
	    Ape.log('read file successfully');
	} else {
	    Ape.log('Could not read file correctly: returned: \n%<-------------\n' + content + '\n%<-------------\n' + 'instead of' + '\n%<-------------\n' + twice + '\n%<-------------\n');
	}
	/*
	/*
	 * to a tempfile
	 * and rewrite the temp file
	 * */
	
	fn = os.writefile('', rot13, true);
	if (fn ) {
	    Ape.log('write succesfully: ' + fn);
	}else{
	    Ape.log('Could not write file: ' + fn);
	}
	content = Ape.os.readfile(fn);
	if (content === rot13) {
	    Ape.log('read file successfully');
	} else {
	    Ape.log('Could not read file: returned: \n%<-------------\n' + content + '\n%<-------------\n' + 'instead of' + '\n%<-------------\n' + rot13 + '\n%<-------------\n');
	}
} catch (e) {
	Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
}
Ape.log('<<< =====================================\n');
