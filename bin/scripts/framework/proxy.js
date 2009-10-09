Ape.registerCmd("PROXY_CONNECT", true, function(params, infos) {
	if (!$defined(params.host) || !$defined(params.port)) {
		return 0;
	}
	var socket = new Ape.sockClient(params.port, params.host);
	
	/* add socket to the user */
	
	socket.onConnect = function() {
		var pipe = new Ape.pipe();
		pipe.link = socket;
		this.pipe = pipe;
		
		pipe.onSend = function(users, params) {
			this.link.write(Ape.base64.encode(params.msg));
		}

		infos.user.pipe.sendRaw("PROXY_EVENT", {"event": "connect"}, {from: this.pipe});
	}
	
	socket.onRead = function(data) {
		infos.user.pipe.sendRaw("PROXY_EVENT", {"event": "read", "data": Ape.base64.encode(data)}, {from: this.pipe});
	}
	
	socket.onDisconnect = function(data) {
		infos.user.pipe.sendRaw("PROXY_EVENT", {"event": "disconnect"}, {from: this.pipe});
		this.pipe.destroy();
	}
	
	return 1;
});
