/*
 * Enhance Ape.log function
 */
Apelog = Ape.log;
Ape.log = function(data){
	switch(typeof data){
		case "object":
			var result = "----" + data + "----\n";
			var level = 0;
			data = data.toSource();

			for(var i= 0; data.length > i; i++){
				var letter = data[i];
				var next = data[i+1];
				var previous = data[i-1];

				var tab = false;

				if(letter != " "){
					result += letter;

					switch(letter){
						case "{":
							if(next != "}"){
								result += "\n";
								level++;
								tab = true;
							}
							break;
						case "}":
							if(previous != "{" && next != "," && next != ")"){
								result += "\n";
								level--;
								tab = true;
							}else if(next == "}"){
								result += "\n";
								level--;
								tab = true;
							}
							break;
						case "[":
							if(next != "]"){
								result += "\n";
								level++;
								tab = true;
							}
							break;
						case "]":
							if(previous != "[" && next != ","){
								result += "\n";
								level--;
								tab = true;
							}else if(next == "]"){
								result += "\n";
								level--;
								tab = true;
							}
							break;

						case ",":
							if(next != "{"){
								result += "\n";
								tab = true;
							}
							break
						case ":":
							result += " ";
							break;

						default:
							if(next == "}" || next == "]"){
								result += "\n";
								level--;
								tab = true;
							}

					}
					if(tab)
						for(var l=0; level > l; l++)
							result += "  ";
				}
			}
			Apelog(result);
			break;
		default:
			Apelog(data);
	}
}
