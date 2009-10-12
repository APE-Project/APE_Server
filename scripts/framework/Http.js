/* Copyright (C) 2009 Weelya & Gasquez Florian <f.gasquez@weelya.com> */

/* APE Http (MonkURL)

	Exemple 2:
		// URL to call
		var request = new Http('http://www.google.fr:80/');
		
		// Action GET or POST
		request.options('action', 'GET');
		
		request.doCall(function(result) {
			Ape.log(result);
		});

	Example 1:
		request = new Http('http://twitter.com:80/statuses/update.json');
		request.options('action', 'POST');
		
		// GET or POST data
		request.options('data', {"status":"Hello!"});
		
		// http authentification (option)
		request.options('auth', 'user:password');
		
		request.doCall(function (result) {
			Ape.log(result);
		});
*/

var Http = new Class({
	version:		0.1,
	action:  		null,
	host:    		null,
	port:	 		null,
	query:	 		null,
	headers: 		null,
	data:			null,
	authKey: 		null,
	returnHeaders:	false,
	lastResponse: 	new Array(),
	currentCall:  	0,

	initialize: function(url) {
		this.url 	= url;
		var result	= url.match("^.*?://(.*?):(.*?)(/.*)$");
		this.host	= result[1];
		this.port	= result[2];
		this.query	= result[3];
	},

	initHeaders: function() {
		if (this.data) {
			this.data = "?" + this.data;
		} else {
			this.data = '';
		}

		this.headers =	this.action + " " + this.query + this.data + " HTTP/1.1\r\nHost: " + this.host + "\r\n";
	},

	setHeaders: function(key, value) {
		this.headers += key + ": " + value + "\r\n";
	},

	/* Options: 
		You need to set options in this order: action/data/auth/returnHeaders.
	*/
	options: function(key, value) {
		if (key == 'auth') {
			this.auth	= 'Basic ' +  Ape.base64.encode(value);
			this.setHeaders('Authorization', this.auth);
		} else if (key == 'data') {
			this.data 	= Hash.toQueryString(value);

			if (this.action == "POST") {
				this.setHeaders('Content-length', this.data.length);
				this.setHeaders('Content-Type', 'application/x-www-form-urlencoded');
			} else {
				this.initHeaders();
			}
		} else if (key == 'action') {
			this.action = value;
			this.initHeaders();
		} else if (key == 'returnHeaders') {
			this.returnHeaders = value;
		}
	},

	doCall: function(callback) {
		this.currentCall++;

		var socket = new Ape.sockClient(this.port, this.host, { flushlf: false });

		this.setHeaders('User-Agent', 'MonkURL '+this.version);
		this.setHeaders('Accept', '*/*');

		socket.onConnect = function() {
			socket.write(this.headers);
			socket.write("\r\n");
			if (this.data && this.action == 'POST') {
				socket.write(this.data);
			}
		}.bind(this);

		socket.onRead = function(data) { 
			this.lastResponse[this.currentCall] += data;
		}.bind(this);

		socket.onDisconnect = function(callback) {
			this.parseResult(this.lastResponse[this.currentCall], callback);
			delete this.lastResponse[this.currentCall];
		}.bind(this, callback);
	},

	parseResult: function(result, callback) {
		if (this.returnHeaders == true) {
			if (callback) {
				callback.run(result);
			}
		} else {
			var result = result.split("\r\n\r\n");
			if (callback) {
				callback.run(result[1], this);
			}
		}
	}
});

