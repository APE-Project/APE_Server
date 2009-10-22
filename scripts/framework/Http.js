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
	receiveHeaders: false,
	lastResponse: 	'',
	currentCall:  	0,

	initialize: function (url) {
		this.url 	= url;
		this.parseURL();
	},

	parseURL: function () {
		var result	= this.url.match("^.*?://(.*?)(:(.*?)|)(/.*)$");
		this.host	= result[1];
		this.query	= result[4];
		if (result[3]) {
			this.port	= result[3];
		}
	},

	/* new urlGetContents */
	getContent: function (callback) {
		this.options('action', 'GET');
		this.doCall(callback);
	},

	/* For compatibility */
	urlGetContents: function (callback) {
		this.getContent(callback);
	},

	/* Build Headers */
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

	/* Exec HTTP request */
	doCall: function (callback) {		
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
			this.lastResponse = '';
		}.bind(this);

		socket.onRead = function(data) { 

			this.lastResponse += data;

			if (this.lastResponse.contains("\r\n\r\n")) {
				this.parseHeaders();
				
				if ($defined(this.headersDetails)) {
					if ($defined(this.headersDetails['Content-Length']) && this.getBufferSize() >= this.headersDetails['Content-Length']) {
						socket.close();
					}
				
					if (this.headersDetails['Location'] != null) {
						socket.close();
						this.redirect = this.headersDetails.get('Location');
					}
				}
			}
		}.bind(this);

		socket.onDisconnect = function(callback) {
			this.parseResult(this.lastResponse, callback);
			delete this.lastResponse;
		}.bind(this, callback);
	},
	
	getBufferSize: function() {
		return this.lastResponse.length-this.headersLength-4;
	},
	
	/* Split headers */
	parseHeaders: function () {
		if (!this.headersDetails) {
			var tmp				= this.lastResponse.split("\r\n\r\n");
			this.headersLength  = tmp[0].length;
			tmp 				= tmp[0].split("\r\n");

			
			this.headersDetails = new Hash();
			for (var i = 1; i < tmp.length; i++) {
				var tmpHeaders = tmp[i].split(": ");
				this.headersDetails.set(tmpHeaders[0], tmpHeaders[1]);
			}
		}
	},

	/* Parsing data */
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
