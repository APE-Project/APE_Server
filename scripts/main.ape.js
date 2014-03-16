Ape.addEvent('init', function() {
	try {
		//This file is essential
		include('framework/mootools.js');

		//These 4 files are recommended
		include('utils/utils.js');
		include('framework/Http.js');
		include('framework/userslist.js');
		include('commands/proxy.js');
		include("framework/log.js"); //Enhanced Ape.log() method, comment this line to disable it.

		//These 5 files are meant for the demo's or serves as examples
		//include('examples/http_auth.js'); //Edit the url in this file to avoid crash
		//include('commands/inlinepush.js');
		//include('examples/nickname.js');
		//include('examples/move.js');
		//include('examples/ircserver.js');


		//This file is needed for the APE JSF diagnostic tool, once APE is installed you can remove it
		include('utils/checkTool.js');

		//This file is used to test features of APE. Especially meant for develompment of APE Features
		//include('test/_tests.js');

	} catch (e) {
		Ape.log(e.message + '\n\t' + e.fileName + ':' + e.lineNumber);
	}
});
