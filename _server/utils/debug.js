function var_dump(obj, name, spaces, ret) {

	var res = '';

	ret = name===true || ret || false;
	name = name && name!==true?name+': ':'';
	spaces = spaces || '';

	var type = $type(obj);


	if(type == 'arguments'){
		var tmp = new Array();
		for(var i = 0;i < obj.length;i++){
			tmp[i] = obj[i];
		}
		obj = tmp;
	}


	if(type == 'object' || type == 'array' || type == 'hash' || type == 'arguments'){
		var brace = type=='array'||type == 'arguments'?'[':'{';
		res += spaces+''+name+'('+type+') '+brace+'\n';
		var first = 0;
		for(var k in obj){
			if(!first) first++
			else res += ',\n'
			if(true || obj.hasOwnProperty(k)){
				res += var_dump(obj[k], k, spaces+'    ', true);
			}
		}
		brace = type=='array'||type == 'arguments'?']':'}';
		res += '\n'+spaces+brace;
	}else{
		if(type===false){
			res += (spaces+''+name+obj);
		}else{
			if(type=='function'){
				obj = '';
			}else if(type!='number'){
				obj = " '"+obj+"'";
			}else{
				obj = ' '+obj;
			}
			res += (spaces+''+name+'('+type+')'+obj);
		}
	}
	if(ret) return res;
	Ape.log(res);
	return true;
}
function var_export(obj, name, spaces, ret) {

	var res = '';

	ret = name===true || ret || false;
	name = name && name!==true?name+':':'';
	spaces = spaces || '';

	var type = $type(obj);

	if(type == 'arguments'){
		var tmp = new Array();
		for(var i = 0;i < obj.length;i++){
			tmp[i] = obj[i];
		}
		obj = tmp;
	}

	if(type == 'object' || type == 'array' || type == 'hash' || type == 'arguments'){
		var brace = type=='array'||type == 'arguments'?'[':'{';
		res += spaces+''+name+brace+'\n';
		var first = 0;
		for(var k in obj){
			if(obj.hasOwnProperty(k)){
				if(!first) first++
				else res += ',\n'
				if(true || obj.hasOwnProperty(k)){
					res += var_export(obj[k], k, spaces+'    ', true);
				}
			}
		}
		brace = type=='array'||type == 'arguments'?']':'}';
		res += '\n'+spaces+brace;
	}else{
		if(type !== false && type!='number'){
			obj = "'"+obj+"'";
		}
		res += spaces+''+name+''+obj;
	}
	if(ret) return res;
	Ape.log(res);
	return true;
}