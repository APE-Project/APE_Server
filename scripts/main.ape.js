Ape.addEvent('init', function() {
	try {
		//This file is essential
		include("framework/mootools.js");
		
		//These 4 files are recommended
		include("utils/utils.js");
		include("framework/Http.js");
		include("framework/userslist.js");
		include("commands/proxy.js");
		
		//These 5 files are meant for the demo's
		//include("framework/http_auth.js");
		//include("commands/inlinepush.js");
		//include("examples/nickname.js");
		//include("examples/move.js");
		//include("examples/ircserver.js");
		
		
		//This file is needed for the APE JSF diagnostic tool, once APE is installed you can remove it
		///include("utils/checkTool.js"); 
		
		//This file is used to test features of APE. Especially meant for develompment of APE Features
		//include("test/_tests.js");
			
	} catch (e) {
		Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
	}
});
