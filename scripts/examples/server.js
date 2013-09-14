/* Listen on port 6970 (multicast) and high performences server */
/* Check ./framework/proxy.js for client API */

var socket = new Ape.sockServer(6970, "0.0.0.0", {
	flushlf: true /* onRead event is fired only when a \n is received (and splitted around it) e.g. foo\nbar\n  will call onRead two times with "foo" and "bar" */
});

/* fired when a client is connecting */
socket.onAccept = function(client) {
	Ape.log("New client");
	client.write("Hello world\n");
	client.foo = "bar"; // Properties are persistants
	//client.close();
}

/* fired when a client send data */
socket.onRead = function(client, data) {
	Ape.log("Data from client : " + data);
}

/* fired when a client has disconnected */
socket.onDisconnect = function(client) {
	Ape.log("A client has disconnected");

}

Ape.log("Listen on port " + port + '...');
	
