(function(){
	function http_auth(url, params, callback)
	{
		var request = new Http(url);
		request.set('method', 'POST');
		request.writeObject(params);
		request.getContent(function(result) {
			var ret = {};
			try { ret = JSON.parse(result); } catch(e){};
		
			callback(ret);
		});	
	}

	Ape.registerHookCmd("CONNECT", function(params, cmd) {
		http_auth("http://127.0.0.1/index.php", params, function(result) {
			if (result == 1) Ape.addUser(cmd.user);
			else cmd.sendResponse("FAIL", {"you":"fail"});
		});
	
		return -1;
	});
})();
