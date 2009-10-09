if (0) {




Ape.addEvent("init", function() {

	Ape.addEvent("adduser", function(user) {
		user.foo = "bar";
	});
	
	Ape.addEvent("deluser", function(user) {
		Ape.log("Del user : " + user.getProperty('nickname'));
	});
	
	Ape.addEvent("afterJoin", function(user, channel) {
		//Ape.log("JOIN !" + channel.getProperty('name'));
		Ape.log(Hash.toQueryString(user.pipe.toObject()));
	});
	
	Ape.addEvent("beforeJoin", function(user, channel) {
		Ape.log("Before...");
	})
	
	Ape.addEvent("mkchan", function(channel) {
		Ape.log("new channel " + channel.getProperty('name'));
	});

	
	/* Create a non-blocking socket that listen on port 7779 */
	
	
	/*var ca = new Ape.sockClient(6667, "irc.freenode.org", {
		flushlf: true
	});
	
	var sockets = create_js_server(7779, ca);
	
	ca.onConnect = function() {
		Ape.log("Socket connected");
		this.write("USER a a a a\n");
		this.write("NICK APE_BoT\n");
		this.write("JOIN #ape-project\n");

	}
	
	ca.onRead = function(data) {
		//Ape.log(data);
	}

	ca.onDisconnect = function() {
		Ape.log("Disconnected");
	}*/
	
	/* Register a CMD that not require user to be loged */
	
	if (0) {
		Ape.registerCmd("webhook", false, function(params, infos){
			var data = {params:params, infos:infos};
		
			/* make a post request */
			Ape.HTTPRequest('http://www.rabol.fr/bordel/post.php', data);
		
			/* Broadcast params to clients connected to the js server */
			clients.each(function(client) {
				Ape.log("Sending to client...");
				client.write(Hash.toQueryString(params));
			});
		
			ca.write("PRIVMSG #ape-project :" + Hash.toQueryString(params) + "\n");
		});

	}

	/*Ape.registerHookCmd("connect", function(params, infos) {
		Ape.log("woot");
	//	Ape.log("Yoh");
	//	ca.write("PRIVMSG #ape-project :New user connected to APE (ip : "+infos.ip+")\n");
	});*/
	
	/* Register a CMD that require user to be loged */
	Ape.registerCmd("foocmd", true, function(params, infos) {
		infos.user.pipe.sendRaw("foo", {"key":"val"}, {
			from: infos.user.pipe
		});
	});

});
}


			/*	Ape.log("Challenge : " + infos.chl);
				Ape.log("Host : " + infos.host);

				Ape.log("User sessid : " + user.getProperty('sessid'));
				Ape.log("User pubid : " + user.getProperty('pubid'));
				Ape.log("User nickname : " + user.getProperty('nickname'));
				Ape.log("User xxx : " + user.xxx);*/

				/*var pipe = Ape.getPipe(user.getProperty('pubid'));
				if ($chk(pipe)) {
					pipe.sendRaw("Kikoo", {"foo":"bar"}, true);
					Ape.log("Send raw JS");
				} else {
					Ape.log("Not found " + pipe);
				}*/
				//var pipe = Ape.getPipe(user.getProperty('pubid'));
/*
	cb.callUser
	cb.callSubUser
	cb.fdClient
	cb.host
	cb.chl
*/
