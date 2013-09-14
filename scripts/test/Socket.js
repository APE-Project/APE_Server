//Create Socket
var socket = new Ape.sockClient('21', 'example.com', {flushlf: true});

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