
Ape.addEvent("init", function() {
	
	/* Fired when a new user is connecting */
	Ape.addEvent("adduser", function(user) {
		user.foo = "bar"; // Private property
		user.setProperty('foo', 'bar'); // Public property (send to ther users)
	});
	
	/* Fired when a user is disconnecting */
	Ape.addEvent("deluser", function(user) {
		Ape.log("Del user ("+ user.foo +") : " + user.getProperty('foo'));
	});
	
	/* Fired just after the user join a channel */
	Ape.addEvent("afterJoin", function(user, channel) {
		Ape.log("JOIN !" + channel.getProperty('name'));
	});
	
	/* Fired just before the user join a channel */
	Ape.addEvent("beforeJoin", function(user, channel) {
		Ape.log("Before...");
	})
	
	/* Fired when a channel is created */
	Ape.addEvent("mkchan", function(channel) {
		Ape.log("new channel " + channel.getProperty('name'));
	});

	
	/* Register a new command  (false : sessid not needed) */
	Ape.registerCmd("webhook", false, function(params, infos) {
		var data = {params:params, infos:infos};
	
		/* make a post request */
		Ape.HTTPRequest('http://www.rabol.fr/bordel/post.php', data);

	});
	
	/* Register a CMD that require user to be loged (sessid needed) */
	Ape.registerCmd("foocmd", true, function(params, infos) {
		/* Send a row to the given pipe (pubid) */
		Ape.getPipe(params.pipe).sendRaw("foo", {"key":"val"}, {
			from: infos.user.pipe /* (optional) User is attached to the raw */
		});
	});
	
	/* Hook an existing command (for instance to add params and fire a BAD_PARAMS) */
	Ape.registerHookCmd("foocmd", function(params, infos) {
		if (!$defined(params.pipe)) {
			return 0; // BAD_PARAMS
		} else {
			return 1;
		}
	});
	
});


