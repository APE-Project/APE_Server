Ape.log(' =====================================');
Ape.log('      Start up for test/Socket.js     ');
Ape.log(' =====================================\n');

//Create Socket
var server = 'example.com';
var port = 21;
var socket = new Ape.sockClient(port, server, {flushlf: true});
Ape.log("[Socket] Connecting to server " + server + " on port " + port);

Ape.log('\n >>> Test for Socket Support. You should see "----[object sockClient]----" next line:');
Ape.log(socket); //Spidermonkey 1.8.5 check

socket.onConnect = function() {
    Ape.log("[Socket] Connected to example.com");
    //this.write("Hello\n"); //Uncomment this if connexion is successfull. If the spidermonkey issue is still there, APE will crash when this file is loaded
}

socket.onRead = function(data) {
    Ape.log("[Socket] Data : " + data);
}

socket.onDisconnect = function() {
    Ape.log("[Socket] Gone !");
}

Ape.log("\n\n");