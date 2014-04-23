Ape.registerCmd("PROXY_CONNECT", true, function(params, infos) {
	if (!$defined(params.host) || !$defined(params.port)) {
		return 0;
	}
	var allowed = Ape.config('proxy.conf', 'allowed_hosts');
	
	if (allowed != 'any') {
		var hosts = allowed.split(',');
		var isallowed = false;

		for (var i = 0; i < hosts.length; i++) {
			var parts = hosts[i].trim().split(':');
			if (parts[0] == params.host) {
				if (parts.length == 2 && parts[1] != params.port) continue;
				isallowed = true;
				break;
			}
		}
		if (!isallowed) return [900, "NOT_ALLOWED"];
	}
	
	var socket = new Ape.sockClient(params.port, params.host);
	socket.chl = infos.chl;
	/* TODO : Add socket to the user */
	
	socket.onConnect = function() {
		/* "this" refer to socket object */
		/* Create a new pipe (with a pubid) */
		var pipe = new Ape.pipe();
		
		infos.user.proxys.set(pipe.getProperty('pubid'), pipe);
		
		/* Set some private properties */
		pipe.link = socket;
		pipe.nouser = false;
		this.pipe = pipe;
		
		/* Called when an user send a "SEND" command on this pipe */
		pipe.onSend = function(user, params) {
			/* "this" refer to the pipe object */
			this.link.write(Ape.base64.decode(unescape(params.msg)));
		}
		
		pipe.onDettach = function() {
			this.link.close();
		}
		
		/* Send a PROXY_EVENT raw to the user and attach the pipe */
		infos.user.pipe.sendRaw("PROXY_EVENT", {"event": "connect", "chl": this.chl}, {from: this.pipe});
	}
	
	socket.onRead = function(data) {
		infos.user.pipe.sendRaw("PROXY_EVENT", {"event": "read", "data": Ape.base64.encode(data)}, {from: this.pipe});
	}
	
	socket.onDisconnect = function(data) {
		if ($defined(this.pipe)) {
			if (!this.pipe.nouser) { /* User is not available anymore */
				infos.user.pipe.sendRaw("PROXY_EVENT", {"event": "disconnect"}, {from: this.pipe});
				infos.user.proxys.erase(this.pipe.getProperty('pubid'));
			}
			/* Destroy the pipe */
			this.pipe.destroy();
		}
	}
	
	return 1;
});

Ape.addEvent("deluser", function(user) {
	user.proxys.each(function(val) {
		val.nouser = true;
		val.onDettach();
	});
});

Ape.addEvent("adduser", function(user) {
	user.proxys = new $H;
})


