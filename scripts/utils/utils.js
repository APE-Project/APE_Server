var ape_http_request = Ape.HTTPRequest;

Ape.HTTPRequest = function(url, data) {
	switch ($type(data)){
		case 'object': case 'hash': data = Hash.toQueryString(data);
	}

	ape_http_request(url, data);
};
