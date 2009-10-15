/* Copyright (C) 2009 Weelya & Gasquez Florian <f.gasquez@weelya.com> */

/* APE Http (MonkURL)

	Exemple 1:
		// URL to call
		var request = new Http('http://www.google.fr:80/');
		
		// Action GET or POST
		request.options('action', 'GET');
		
		request.doCall(function(result) {
			Ape.log(result);
		});

	Example 2:
		request = new Http('http://twitter.com:80/statuses/update.json');
		request.options('action', 'POST');
		
		// GET or POST data
		request.options('data', {"status":"Hello!"});
		
		// http authentification (option)
		request.options('auth', 'user:password');
		
		request.doCall(function (result) {
			Ape.log(result);
		});
		
	Example 3:
		request = new Http('http://www.google.com/');
		request.urlGetContents(function (result) {
			Ape.log(result);
		});
*/

var Http = new Class({
	version:		0.1,
	action:  		null,
	host:    		null,
	port:	 		80,
	query:	 		null,
	headers: 		null,
	data:			null,
	authKey: 		null,
	returnHeaders:	false,
	redirected:		false,
	headersDetails: [],
	receiveHeaders: [],
	lastResponse: 	[],
	currentCall:  	0,

	initialize: function (url) {
		this.url 	= url;
		this.parseURL();
	},

	parseURL: function() {
		var result	= this.url.match("^.*?://(.*?)(:(.*?)|)(/.*)$");
		this.host	= result[1];
		this.query	= result[4];
		if (result[3]) {
			this.port	= result[3];
		}
	},

	urlGetContents: function(callback) {
		this.options('action', 'GET');
		this.doCall(callback);
	},

	initHeaders: function () {
		if (this.data) {
			this.data = "?" + this.data;
		} else {
			this.data = '';
		}

		this.headers =	this.action + " " + this.query + this.data + " HTTP/1.0\r\nHost: " + this.host + "\r\n";
	},

	setHeaders: function (key, value) {
		this.headers += key + ": " + value + "\r\n";
	},

	/* Options: 
		You need to set options in this order: action/data/auth/returnHeaders.
	*/
	options: function (key, value) {
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

	doCall: function (callback) {
		this.currentCall++;
		
		if (!this.action) {
			this.actions('action', 'GET');
		}

		var socket = new Ape.sockClient(this.port, this.host, { flushlf: false });

		this.setHeaders('User-Agent', 'MonkURL '+this.version);
		this.setHeaders('Accept', '*/*');

		socket.onConnect = function() {
			socket.write(this.headers+"\r\n");
			if (this.data && this.action == 'POST') {
				socket.write(this.data);
			}
			this.lastResponse[this.currentCall] = '';
		}.bind(this);

		socket.onRead = function(data) { 
			this.lastResponse[this.currentCall] += data;

			if (this.lastResponse[this.currentCall].contains("\r\n\r\n")) {
				this.parseHeaders(this.currentCall);
				if (this.headersDetails[this.currentCall].get('Content-Length') != null && this.lastResponse[this.currentCall].length > this.headersDetails[this.currentCall].get('Content-Length')) {
					socket.close();
				}
				if (this.headersDetails[this.currentCall].get('Location') != null) {
					socket.close();
					this.redirect = this.headersDetails[this.currentCall].get('Location');
				}
			}
		}.bind(this);

		socket.onDisconnect = function(callback) {
			this.parseResult(this.lastResponse[this.currentCall], callback);
			delete this.lastResponse[this.currentCall];
		}.bind(this, callback);
	},
	
	parseHeaders: function (data) {
		var tmp		= this.lastResponse[this.currentCall].split("\r\n\r\n");
		tmp 		= tmp[0].split("\r\n");

		if (!this.headersDetails[this.currentCall]) {
			this.headersDetails[this.currentCall] = new Hash();
			for (var i = 1; i < tmp.length; i++) {
				var tmpHeaders = tmp[i].split(": ");
				this.headersDetails[this.currentCall].set(tmpHeaders[0], tmpHeaders[1]);
			}
		}
	},

	parseResult: function (result, callback) {
		if (!this.redirect) {
			var parseResult = result.split("\r\n\r\n");

			if (this.returnHeaders == true) {
				if (callback) {
					callback.run(result);
				}
			} else {
				if (callback) {
					var returnResult = '';
					for (var i = 1; i < parseResult.length; i++) {
						returnResult += parseResult[i];
					}
					callback.run(returnResult, this);
				}
			}
		} else {
			newRequest = new Http(this.redirect);
			newRequest.options('action', this.action);
			newRequest.options('data', this.data);
			newRequest.doCall(callback);
		}
	}
});
