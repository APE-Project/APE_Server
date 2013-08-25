Ape.addEvent("init", function() {
	include("framework/mootools.js");
	include("framework/Http.js");
	include("framework/userslist.js");
	include("framework/log.js"); //Enhanced Ape.log() method, comment this line to disable it.
	include("utils/utils.js");
	include("commands/proxy.js");
	include("commands/inlinepush.js");
	include("examples/nickname.js");
	include("examples/move.js");
	include("utils/checkTool.js"); //Just needed for the APE JSF diagnostic tool, once APE is installed you can remove it 
	//include("examples/ircserver.js");
	//include("framework/http_auth.js");
	
	//Those file are used to test feature of APE.
	//include("test/MySQL.js");
	//include("test/Socket.js");
	//include("test/Pipes.js");
});
