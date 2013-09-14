Ape.addEvent('init', function() {
	try {
		include('test/Systemcmd.js');
		include('test/Hostname.js');
		include('test/MySQL.js');
		include('test/Socket.js');
		include('test/Pipes.js');
		//include('test/MeteorLeak.js');//this test will harm your server!!!
		include('test/Eval.js');
		include('test/Properties.js');
		include('test/FileReadWrite.js');
		include('test/Status.js');
	} catch (e) {
		Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
	}
});
