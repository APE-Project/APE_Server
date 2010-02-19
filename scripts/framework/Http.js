/* Copyright (C) 2009 Weelya & Gasquez Florian <f.gasquez@weelya.com> */

/*
Exemple 1:
	// URL to call
	var request = new Http('http://www.google.fr/');
	request.getContent(function(result) {
		Ape.log(result);
	});

Example 2:
	var request = new Http('http://twitter.com:80/statuses/update.json');
	request.set('method', 'POST');
	
	// GET or POST data
	request.writeData('status', 'Hello!');
	
	// HTTP Auth
	request.set('auth', 'user:password');
	
	request.getContent(function (result) {
		Ape.log(result);
	});
	
Example 3:
	// URL to call
	var request = new Http('http://www.google.fr/');
	request.finish(function(result) {
		// {status:Integer, headers:Array, body:String}
	});
*/

var Http = new Class({
	method:		'GET',
	headers:	[],
	body:		[],
	url:		null,
	
	initialize: function(url, port) {
		this.url 			= url;
		this.port			= port || 80;
		
		this.parseURL();
	},
	
	parseURL: function() {
		var result  = this.url.match("^.*?://(.*?)(:([0-9]+))?((/.*)|)$");
		this.host   = result[1];
		this.port   = result[3] || 80;
		this.query  = result[4];
	},
	
	set: function(key, value) {
		if (key == 'auth') {
			this.auth	= 'Basic ' +  Ape.base64.encode(value);
			this.setHeader('Authorization', this.auth);
		} else {
			this[key] = value;
		}
	},
	
	setHeader: function(key, value) {
		this.headers[key] = value;
	},
	
	setHeaders: function(object) {
		this.headers = object;
	},
	
	write: function(data) {
		this.body.push(data);
	},
	
	writeData: function(key, value) {
		var tmpData = {};
		tmpData[key] = value;
		this.write(Hash.toQueryString(tmpData));
	},
	
	writeObject: function(data) {
		this.write(Hash.toQueryString(data));
	},
	
	getContentSize: function() {
		return this.response.length-this.responseHeadersLength-4;
	},
	
	connect: function() {		
		if (this.method == 'POST') {
			this.setHeader('Content-length', this.body.join('&').length);
			this.setHeader('Content-Type', 'application/x-www-form-urlencoded');
		}

		this.setHeader('User-Agent', 'APE JS Client');
		this.setHeader('Accept', '*/*');
		
		this.socket = new Ape.sockClient(this.port, this.host, { flushlf: false });
		
		this.sockConnect();
		this.sockRead();
	},
	
	sockConnect: function() {
		this.socket.onConnect = function() {
			if (this.body.length != 0 && this.method == 'GET') {
				var getData = '';
			
				if (this.method == 'GET') {
					getData = '?' + this.body.join('&');
				}
			}

			var toWrite = this.method + " " + this.query + " HTTP/1.0\r\nHost: " + this.host + "\r\n";
			
			for (var i in this.headers) {
				if (this.headers.hasOwnProperty(i)) {
					toWrite += i + ': ' + this.headers[i] + "\r\n";
				}
			}

			this.socket.write(toWrite + "\r\n");
			this.socket.write(this.body.join('&'));
		}.bind(this);
	},
	
	sockRead: function() {
		this.response = '';
		this.socket.onRead = function(data) { 
			this.response += data;
			if (this.response.contains("\r\n\r\n")) {
				if (!$defined(this.responseHeaders)) {
					var tmp						= this.response.split("\r\n\r\n");
					this.responseHeadersLength 	= tmp[0].length;
					tmp 						= tmp[0].split("\r\n");
					this.responseHeaders 		= [];
					this.responseCode			= tmp[0].split(" ");
					this.responseCode			= this.responseCode[1].toInt();

					for (var i = 1; i < tmp.length; i++) {
						var tmpHeaders = tmp[i].split(": ");
						this.responseHeaders[tmpHeaders[0]] = tmpHeaders[1];
					}
				} else {
					if ($defined(this.responseHeaders['Content-Length']) && this.getContentSize() >= this.responseHeaders['Content-Length']) {
						this.socket.close();
					} 
					if ($defined(this.responseHeaders['Location'])) {
						socket.close();
					}
				}
			}				
		}.bind(this);
	},
	
	read: function(callback) {
		this.socket.onDisconnect = function(callback) {
			this.response	  	 = this.response.split("\r\n\r\n");
			this.response.shift();
			this.response	  	 = this.response.join();
			this.httpResponse 	 = {status:this.responseCode, headers:this.responseHeaders, body:this.response};
			
			if ($defined(this.responseHeaders)) {
				if ($defined(this.responseHeaders['Location'])) {
					var newRequest   = new Http(this.responseHeaders['Location']);
					newRequest.setHeaders(this.headers);
					newRequest.set('method', this.method);
					newRequest.write(this.body.join('&'));
					newRequest.finish(callback);
				} else {
					callback.run(this.httpResponse);
				}
			}
		}.bind(this, callback);
	},
	
	finish: function(callback) {
		this.connect();
		this.read(callback);
	},
	
	getContent: function (callback) {
		this.connect();
		this.read(function(result) {
			callback.run(result['body']);
		});
	},
});