APE.IrcClient.Channel = new Class({
    name: false,
    dontclose:false,
    messages: new Array(),
    users: false,
    last_user: '',
    irc: null,
    line_size: 83,
    history: new Array(),
    MAX_HISTORY: 100,
    Hcnt: 0,
    initialize: function(name, irc, dontclose){
        this.dontclose = dontclose===true;
        this.name = name;
        this.irc = irc;
        this.plop = rand_chars();
		this.users = new Hash();
    },
    addUser: function(username, sort){
		
        var type;
        switch(username.substr(0,1)){
            case '@':
                type = 'operator';
                break;
            case '+':
                type = 'voice';
                break;
            default:
                type = 'zzz';
        }

        if(type != 'zzz' && this.users.has(username.substr(1))){
            this.renameUser(username.substr(1), username);
            this.users.get(username.type = type);
            return 'rename';
        }

        if(this.users.has(String(username))){
            return false;
        }
        this.users.set(username, {
            'nick': username,
            'type': type
        });
        this.last_user = username;
        if(type != 'zzz' && sort !== false) this.sortUsers();
        return true;
    },
    sortUsers: function(){
        var a_users = this.users.getValues();
        a_users.sort(this.compareUsers);
        this.users.empty();
        a_users.each(function(usr){
            this.users.set(usr.nick, usr);
        }.bind(this));
    },
    compareUsers: function(a, b){
        if(a.type == b.type){
            return 0;
        }else if(a.type > b.type){
            return 1;
        }else{
            return -1
        }
    },
    remUser: function(username){
        if(this.users.has(username)){
            this.users.erase(username);
            return true;
        }

        return false;
    },
    renameUser: function(from, to){
        if(this.users.has(from)){
            //var user = this.users.get(from);
            this.remUser(from);
            this.addUser(to);
            return true;
        }else if(this.users.has('@'+from)){
            return this.renameUser('@'+from, (to.substr(0,1)=='@'||to.substr(0,1)=='+')?to:'@'+to);
        }else if(this.users.has('+'+from)){
            return this.renameUser('+'+from, (to.substr(0,1)=='@'||to.substr(0,1)=='+')?to:'+'+to);
        }
        return false;
    },
    getLastUser: function(){
        return this.users.get(this.last_user);
    },
    search: function(str){
        var ret = false;
        this.users.each(function(usr){
            var start = 0;
            if(usr.nick.charAt(0)=='@' || usr.nick.charAt(0)=='+') start = 1;
            if(usr.nick.substr(start, str.length) == str){
                ret = usr.nick.substr(start);
            }
        }.bind(this));
        return ret;
    },
    userCount: function(){
        return this.users.getLength();
    },
    addMessage: function(from, msg, special, user){

        var txt = new Array();
        
        msg = $type(msg)=='array'?msg:[msg];
        msg.each(function(item){
            if($type(item)!='element'){
                txt = txt.concat(this.parseText(String(item)));
            }else{
                txt.push(item);
            }
        }.bind(this))

        var message = {
            'name': from,
            'msg' : txt,
            'date': new Date(),
            'special': special,
            'usr': user
        };
        this.messages.push(this.makeMessageLine(message));

        if(this.messages.length > this.MAX_HISTORY) {
            var destroy = this.messages.slice(0, this.messages.length - this.MAX_HISTORY);
            this.messages = this.messages.slice(-this.MAX_HISTORY);
            destroy.each(function(el){
                el.destroy();
            })
        }
    },
    makeMessageLine: function(message){
        /*
        <div class="msg_line [error|info|user-left|notice|action|topic|highlight|ctcp]">
            <div class="msg_user">
                <pre>[12:30]    Username</pre>
            </div>
            <pre class="msg_text">
                Txt of the message
            </pre>
        </div>
        me "plop" chan name must begin with # or & and must not contains any of ' ' (sp
         */


        var el_txt = this.splitText(message);

        //Highlighting
        if(message.usr != undefined){
            var reg = new RegExp('@?'+message.usr+'([^a-z~0-9\-_]|$)', 'i');
            if(el_txt.get('text').match(reg)){
                message.special = message.special==undefined?'highlight':message.special+' highlight';
            }
        }

        var el_line = new Element('div', {
            'class': 'msg_line'+(message.special?' '+message.special:'')
        });

        var el_divu = new Element('div', {
            'style': 'width:'+this.getLineWidth()+'px',
            'class': 'msg_user'
        });
        var hours = message.date.getHours();
        hours = hours > 9 ? hours : '0'+hours;
        var min = message.date.getMinutes();
        min = min > 9 ? min : '0'+min;

        var el_user = new Element('pre', {
            'text': '['+hours+':'+min+']  '+($type(message.name)!="element"?message.name:'')
        });
        if($type(message.name)=='element') el_user.grab(message.name);

        el_divu.grab(el_user);

        el_line.grab(el_divu);
        el_line.grab(el_txt);

        return el_line;
    },
    join: function(){
        if(this.irc)
            this.irc.join(this.name);
    },
    splitWords: function(item){
        var ret = new Array();
        var tmp = item.split(' ');
        tmp.each(function(word, k, list){
            ret.push( word + (k<tmp.length-1?' ':'') );
        }.bind(this));
        return ret;
    },
    splitText: function(message){
        var txt = message.msg;
        if($type(txt)!='array'){
            txt = [txt];
        }
        var ret = new Element('pre', {
            'class': 'msg_text'
        });


        var words = new Array();

        txt.each(function(item){
            if($type(item)=='string'){
                words = words.concat(this.splitWords(item));
            }else{
                words.push(item);
            }
        }.bind(this));

        var cur_size = 0;
        var i = 0;
        words.each(function(word, key){
            var length;
            var type = $type(word);

            if(type=='element'){
                length = word.get('text').length;
            }else{
                length = String(word).replace(/<[^>]*>/g, '').length;
            }

            if(cur_size + length > this.line_size){
                if(cur_size < this.line_size /2 && type!='element'){
                    while(word.length > this.line_size - cur_size){
                        ret.appendText(String(word).substr(0, this.line_size - cur_size -1)+'-');
                        word = String(word).substr(this.line_size - cur_size - 1);
                        ret.appendText('\n');
                        cur_size = 0;
                    }
                }
                ret.appendText('\n');
                cur_size = 0;
            }

            if(type=='element'){
                ret.grab(word);
            }else{
                ret.appendText(String(word));
            }
            cur_size += length;
        }.bind(this));
        return ret;
    },
    buildUsers: function(func){
        var ret = new Array();
        this.users.each(function(item, key){
            ret.push(this.buildUser(key, func));
        }.bind(this));
        return ret;
    },
    buildUser: function(usr, func){

        /*
        <a class="user-item on|afk|off">
            user_name
        </a>
         */
        var user = this.users.get(usr);

        var el_usr = new Element('a', {
            'class': 'user-item '+user.type,
            'id': 'user_line_'+user.nick,
            'text': user.nick,
            'href': '#'
        });
        el_usr.addEvent('click', function(ev, nick) { func(ev, user.nick)}.bindWithEvent(this, user.nick));
        return el_usr;
    },
    buildLastUser: function(func){
        return this.buildUser(this.last_user, func);
    },
    buildMessages: function(){
        return this.messages;
    },
    buildMessage: function(msg){
        return this.messages[msg];
    },
    send: function(msg){
        if(this.irc)
            this.irc.privmsg(this.name, msg);
    },
    clear: function(){
        this.messages = new Array();
        this.history = new Array();
    },
    buildLastMessage: function(){
        return this.buildMessage(this.messages.length -1);
    },
    hasIrc: function(){
        return this.irc?true:false;
    },
    getMaxNicklength: function(){
        // TODO Mise en cache du max et maj a l'ajout de messages
        var ret = 0;
        if(this.messages.length == 0) return 8;
        this.messages.each(function(msg){
            var txt = msg.getElement('.msg_user').get('text');
            ret = Math.max(txt.length-9, ret);
        });
        return Math.max(ret, 8);
    },
    getLineWidth: function(){
        return (this.getMaxNicklength()+10)*6;
    },
    parseText: function(txt){
        txt = txt.replace('\03', '');
        //Fo freenode special "cotes"
        txt = txt.replace(/\02/g, '"');
        txt = txt.replace(/([a-z]{2,6}:\/\/[a-z0-9\-_#\/*+%.~,?&=]+)/gi, '\03$1\04$1\03');
        txt = txt.replace(/([^a-z0-9\-_.\/]|^)((?:[a-z0-9\-_]\.?)*[a-z0-9\-_]\.(?:com|arpa|asia|pro|tel|travel|jobs|edu|gov|int|mill|net|org|biz|arpa|info|name|pro|aero|coop|museum|mobi|[a-z]{2})(?:\/[a-z0-9\-_#\/*+%.~,?&=]*)?)([^a-z]|$)/gi, '$1\03http://$2\04$2\03');

        //Contains URLs
        if(txt.contains('\03')){
            var tmp = new Array();
            txt = txt.split('\03');
            txt.each(function(item){
                if(!item.contains('\04')){
                    tmp.push(item);
                }else{
                    var link = item.split('\04');
                    tmp.push(new Element('a', {
                        'href': link[0],
                        'text': link[1]
                    }));
                }
            });
            return tmp;
        }
        return [txt];
    },
    close: function(reason){
        if(this.irc && !this.dontclose){
            this.irc.part(this.name, reason);
        }
    }
});
