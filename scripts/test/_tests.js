Ape.addEvent('init', function() {
	try {
		Ape.log('\n\n======================================');
		Ape.log('======================================');
		Ape.log('     STARTING UP APE SERVER TESTS     ');
		Ape.log('======================================')
		Ape.log('======================================\n');
		
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
		
		Ape.log('\n\n======================================');
		Ape.log('======================================');
		Ape.log('      APE SERVER TESTS ALL DONE !     ');
		Ape.log('======================================')
		Ape.log('======================================\n');
	} catch (e) {
		Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
	}
});
