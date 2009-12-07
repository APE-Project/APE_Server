
var IRCApeclient = new Class({
	socket_listener: false,
	sessid: false,
	chl: 0,
	client: false,
	
	initialize: function(nickname, client) {
		this.socket_listener = new Ape.sockClient('6969', '127.0.0.1', {flushlf: true});
		this.client = client;
		
		this.socket_listener.onConnect = function() {
			this.sendCmd('CONNECT', {name: nickname}, this.socket_listener);
		}.bind(this);
		
		this.socket_listener.onRead = function(data) {
			data = data.trim();
			if (data[0] == '[') {
				var jsobj = eval(data);
				jsobj.each(this.parseRaw.bind(this));
			}
		}.bind(this);
		
		this.socket_listener.onDisconnect = function() {
		//	Ape.log('Listener disco');
		}
		
		Ape.setInterval(function(obj){
			obj.sendCmd('CHECK', {'null':'null'});
		}, 20000, this);
	},
	
	sendCmd: function(cmdname, obj, sock) {
		
		var mainobj = {'cmd':cmdname, 'chl':this.chl, params:obj};
		if (this.sessid) mainobj.sessid = this.sessid;

		this.chl++;

		var jsonobj = JSON.stringify([mainobj]);

		if (!sock) {
			var control = new Ape.sockClient('6969', '127.0.0.1', {flushlf: true});
			Ape.log('12.12.12.1');
			Ape.log("SEND CMD " + control);
		
			control.onConnect = function() {
				this.write('GET /1/?'+jsonobj+' HTTP/1.1\n');
				this.write('Host: 0\n\n');	
			}
		
			control.onRead = function(data) {
				data = data.trim();
				if (data[0] == '[') {
					var jsobj = eval(data);
					jsobj.each(this.parseRaw.bind(this));
				}
			}.bind(this);
		
			control.onDisconnect = function() {
			//	Ape.log('Controler disco');
			}
		} else {
			sock.write('GET /1/?'+jsonobj+' HTTP/1.1\n');
			sock.write('Host: 0\n\n');			
		}
	},
	
	parseRaw: function(obj) {
	//	Ape.log('Recu : ' + obj.raw);
		if ($defined(this['raw' + obj.raw])) this['raw' + obj.raw](obj.data);
	},
	
	rawLOGIN: function(data) {
		this.sessid = data.sessid;
		this.client.send('NOTICE APE :*** Your sessid ' + this.sessid);
	},
	
	rawCHANNEL: function(data) {
		/*
		:para_!n=para@local.weelya.com JOIN :#ape-project-fr
		:wolfe.freenode.net 332 para_ #ape-project-fr :APE roxorise
		:wolfe.freenode.net 333 para_ #ape-project-fr Lowan 1244582828
		:wolfe.freenode.net 353 para_ @ #ape-project-fr :para_ jchavarria_work Korri1 paraboul jchavarria korri efyx Fy- Lowan 
		:wolfe.freenode.net 366 para_ #ape-project-fr :End of /NAMES list.		
		*/
		var users = [];
		data.users.each(function(user) {
			users.push(user.properties.name);
		})
		this.client.send(':'+this.client.nickname+'!n=ape@weelya.ca.rox.grave JOIN :#'+data.pipe.properties.name);
		this.client.serverRaw('332', 'Tes sur un channel APE mec', '#'+data.pipe.properties.name);
		this.client.serverRaw('353', users.join(' '), '@ #'+data.pipe.properties.name);
		this.client.serverRaw('366', 'End of /NAMES list.', '#'+data.pipe.properties.name);
	},
	
	rawJOIN: function(data) {
		this.client.send(':'+data.user.properties.name+'!n=ape@weelya.ca.rox.grave JOIN :#'+data.pipe.properties.name);
	},
	
	rawERR: function(data) {
		Ape.log('ERREUR : ' + data.value);
	},
	
	rawDATA: function(data) {
		//:efyx_!n=efyx@lap34-1-82-225-185-146.fbx.proxad.net PRIVMSG #ape-project :-test for paraboul
		this.client.send(':'+data.from.properties.name+'!n=ape@weelya.ca.rox.grave PRIVMSG #'+data.pipe.properties.name+' :-'+decodeURIComponent(data.msg));
	}
	
});


var IRCClient = new Class({
	socket: false,
	nickname: false,
	connected: false,
	serverobj: false,
	apeclient: false,
	
	initialize: function(socket, serverobj) {
		this.socket = socket;
		this.serverobj = serverobj;

	},
	
	serverRaw: function(number, msg, more) {
		if (!this.connected) return false;
		this.send(":"+this.serverobj.servername+" "+number+" "+this.nickname+" "+(more ? more : '')+" :"+msg);
	},
	
	send: function(data) {
		//Ape.log(data);
		this.socket.write(data + "\n");
	},
	
	setNick: function(nick) {
		this.nickname = nick;
		
		if (!this.connected) {
			
			this.apeclient = new IRCApeclient(nick, this);
			this.connected = true;
			this.serverRaw('001', 'Welcome to the APE IRC Network ' + nick);
			this.serverRaw('002', 'Your host is '+this.serverobj.servername+'['+this.serverobj.servername+'/'+this.serverobj.port+'], running version APE Server Beta 3');
			this.serverRaw('375', '- APE Message of the Day - ');
			this.serverRaw('372', '-     _    ____  _____   ___ ____   ____ ');
			this.serverRaw('372', '-    / \\  |  _ \\| ____| |_ _|  _ \\ / ___|');
			this.serverRaw('372', '-   / _ \\ | |_) |  _|    | || |_) | |    ');
			this.serverRaw('372', '-  / ___ \\|  __/| |___   | ||  _ <| |___ ');
			this.serverRaw('372', '- /_/   \\_\\_|   |_____| |___|_| \\_\\\\____|');
			this.serverRaw('376', 'End of /MOTD command.');
			this.serverRaw('290', 'IDENTIFY-MSG');
			
		}
	},
	
	joinChan: function(chan) {
		this.apeclient.sendCmd('JOIN', {channels:chan});
	},
	
	privMsg: function(pipe, data) {
		if (pipe[0] == '#') {
			var pubid = Ape.getChannelByName(pipe.substr(1)).getProperty('pubid');
			this.apeclient.sendCmd('SEND', {pipe:pubid, msg:data});
		}
		
	}
	
})


var IRC = new Class({
	port: 6667,
	servername: 'apeirc.ape-project.org',
	
	initialize: function () {
		var irc = new Ape.sockServer('6667', '0.0.0.0', {flushlf: true});
		
		irc.onAccept = function(client) {
			this.onAccept(client);
		}.bind(this);
		
		irc.onRead = function(client, data) {
			this.onRead(client.obj, data.trim());
		}.bind(this);
		
		irc.onDisconnect = function(client) {
			this.onDisconnect(client);
		}.bind(this);
		
	},
	
	onAccept: function(client) {
		client.obj = new IRCClient(client, this);

		client.obj.send("NOTICE AUTH :*** Looking up your hostname...");
		client.obj.send("NOTICE AUTH :*** Checking ident");
		client.obj.send("NOTICE AUTH :*** No identd (auth) response");
		client.obj.send("NOTICE AUTH :*** Found your hostname");

	},
	
	onRead: function(clientobj, data) {
		Ape.log(data);
		var raw = data.split(' ');
		if ($defined(this['cmd' + raw[0]])) {
			var cmd = raw[0];
			raw.shift();
			this['cmd' + cmd](clientobj, raw.join(' '));
		}
	},
	
	onDisconnect: function(client) {
		
	},

	cmdNICK: function(clientobj, nick) {
		clientobj.setNick(nick);
	},
	
	cmdJOIN: function(clientobj, chan) {
		clientobj.joinChan(chan.substr(1));
	},
	
	cmdPRIVMSG: function(clientobj, data) {
		var msg = data.split(':');
		var pipe = data.split(' ');
		msg.shift();
		msg = encodeURIComponent(msg.join(':'));
		clientobj.privMsg(pipe[0], msg);
	}
	
});

Ape.log('Starting IRC server...');
new IRC();
