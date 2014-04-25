Ape.log(' =====================================');
Ape.log('       Start up for test/Pipes.js     ');
Ape.log(' =====================================\n');

//We create a new custom pipe
var pipe = new Ape.pipe();

Ape.log('\n >>> Test for Pipe Support. You should see "----[object pipe]----" next line:');
Ape.log(pipe); //Spidermonkey 1.8.5 check

//Custom pipe is created with an unique pubid
Ape.log('\n >>> Created a pipe with pubid: '+pipe.getProperty('pubid'));

//We listen "SEND" commands received on this pipe
pipe.onSend = function(user, params) {
    Ape.log('Received data from custom pipe: '+params.msg);
    if(params.destroy) {
            pipe.destroy();
    }
}

Ape.log("\n\n");