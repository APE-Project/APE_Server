//We create a new custom pipe
var pipe = new Ape.pipe();

Ape.log(pipe); //Spidermonkey 1.8.5 check

//Custom pipe is created with an unique pubid
Ape.log('Custom pipe pubid: '+pipe.getProperty('pubid'));

//We listen "SEND" commands received on this pipe
pipe.onSend = function(user, params) {
    Ape.log('Received data from custom pipe: '+params.msg);
    if(params.destroy) {
            pipe.destroy();
    }
}