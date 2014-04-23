Ape.addEvent("mkchan", function(channel) {
	channel.userslist = new $H;
});

Ape.addEvent("afterJoin", function(user, channel) {
	channel.userslist.set(user.getProperty('pubid'), user);
});

Ape.addEvent("left", function(user, channel) {
	channel.userslist.erase(user.getProperty('pubid'));
});

Ape.registerCmd("list", false, function(params, infos) {
	
});
