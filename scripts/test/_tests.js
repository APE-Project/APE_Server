Ape.addEvent("init", function() {
	try {
		include("test/Systemcmd.js");
		//include("test/MySQL.js");
		//include("test/Socket.js");
		//include("test/Pipes.js");
	} catch (e) {
		Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
	}
});
