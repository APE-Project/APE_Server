Ape.addEvent("init", function() {
	try {
			include("framework/mootools.js");
			include("framework/Http.js");
			include("framework/userslist.js");
			include("utils/utils.js");
			include("commands/proxy.js");
			include("commands/inlinepush.js");
			include("examples/nickname.js");
			include("examples/move.js");
			include("utils/checkTool.js"); //Just needed for the APE JSF diagnostic tool, once APE is installed you can remove it 
			//include("examples/ircserver.js");
			//include("framework/http_auth.js");
			
			//This file are used to test feature of APE.
			include("test/_tests.js");
	} catch (e) {
		Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
	}
});
