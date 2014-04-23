
include('framework/mootools.js');
include('utils/debug.js');

var ApeMMO_Server = new Class({
	//Properties
	channel: 'apemmo',
	pipe: false,
	userTypeCount: 7,
	creepTypeCount: 2,
	spells: {
		'fire': {
			cooldown: 1000,
			id: 1,
			incant: 0,
			power: 20
		},
		'thunder': {
			cooldown: 2000,
			id: 2,
			incant: 2000,
			power: 50
		}
	},
	creeps: new Hash(),
	creep_id: 0,
	pas: 2,
	//Init
	initialize: function(){
		Ape.log('[Module] Ape mmo started !');
		this.registerEvents();
		this.registerCommands();
		this.addCreep();
		this.addCreep();
		this.addCreep();
		this.addCreep();
	},
	//Functions
	registerEvents: function(){
		Ape.addEvent('mkchan', this.mkchan.bind(this));
		Ape.addEvent('beforeJoin', this.beforeJoin.bind(this));
		Ape.addEvent('afterJoin', this.afterJoin.bind(this));
	},
	registerCommands: function(){
		Ape.registerCmd('mmo_start', true, this.cmdStart.bind(this));
		Ape.registerCmd('mmo_stop', true, this.cmdStop.bind(this));
		Ape.registerCmd('mmo_spell', true, this.cmdMmoSpell.bind(this));
	},
	sendError: function (user, msg, opt){
		var opt = opt || {};
		opt.msg = msg;
		user.pipe.sendRaw('mmo_error', opt);
	},
	addCreep: function (){
		var type = $random(1,this.creepTypeCount);
		var creep = {
			type: type,
			pos: {
				x: $random(100,800),
				y: $random(100,800)
			},
			target: false,
			id: ++this.creep_id,
			totalLife: 100*type,
			'private': (function(){
				var vals = new Hash();
				return function(name, value){
					if(value) vals.set(name, value);
					return vals.get(name);
				};
			})()
		};
		creep.life = creep.totalLife;
		this.creeps.set(this.creep_id, creep);
		Ape.setTimeout(this.creepWalk.bind(this, [this.creep_id]), $random(1000,5000));

		if(this.pipe){
			this.pipe.sendRaw('mmo_creep', {creep:creep});
		}
	},
	updateCreeps: function(){
		this.creeps.each(function(creep){
			//if(creep.target) creep.pos = creep.target;
			if(creep.target && creep.target.x != creep.pos.x &&  creep.target.x != creep.pos.x){
				var s = creep.private('started');
				var now = new Date().getTime();

				while(s.at < now){
					if(creep.pos.x < creep.target.x){
						creep.pos.x += Math.min(this.pas/2, creep.target.x-creep.pos.x);
					}else if(creep.pos.x > creep.target.x){
						creep.pos.x += Math.max(-this.pas/2, creep.target.x-creep.pos.x);
					}else if(creep.pos.y < creep.target.y){
						creep.pos.y += Math.min(this.pas/2, creep.target.y-creep.pos.y);
					}else if(creep.pos.y > creep.target.y){
						creep.pos.y += Math.max(-this.pas/2, creep.target.y-creep.pos.y);
					}else{
						creep.pos = creep.target;
					}
					s.at += 20;
				}
			}
		}.bind(this));
	},
	creepWalk: function(id){
		if(this.creeps.has(id)){
			var creep = this.creeps.get(id);
			if(creep.target){
				creep.pos = creep.target;
				//Ape.log(this.creeps[creep].id+' started at '+this.creeps[creep].private('started')+', '+(new Date().getTime() - this.creeps[creep].private('started'))+'ms ago !');
			}
			var target = {
				x: $random(100,900),
				y: $random(100,900)
			}
			var distance =
				Math.abs(creep.pos.x - target.x) +
				Math.abs(creep.pos.y - target.y);

			creep.target = target;
			if(this.pipe){
				this.pipe.sendRaw('mmo_creep_walk', {creep:id,target:target});
			}
			creep.private('started', {
				at: new Date().getTime(),
				distance: distance
			});
			Ape.setTimeout(this.creepWalk.bind(this, [id]), distance*20/(this.pas/2) + $random(1000,4000));

		}
	},
	endIncant: function (user, spell, target){

		var s = this.spells[spell];

		if(!s){
			Ape.log('Casting unknow spell'+spell);
		}else{

			var power = $random(Math.floor(s.power * 0.8), Math.ceil(s.power * 1.2));

			if(target.substr(0, 6)=='creep_'){
				var id = Number(target.substr(6));

				if(this.creeps.has(id)){
					var c = this.creeps.get(id);
					c.life -= power;
					//Ape.log('creep_'+id+' has got '+c.life+' life points');
					if(c.life <= 0 ){
						//Ape.log('creep_'+id+' is dead');
						this.creeps.erase(id);
						this.pipe.sendRaw('mmo_creep_kill', {creep: id});
						this.addCreep.delay($random(1000,4000), this);
					}
				}else{
					Ape.log('unknow creep '+target);
				}
			}else{
				var usr = Ape.getUserByPubid(target);
				if(usr){
					//Ape.log('Dammaging '+target+' -'+power+'lp');
					usr.setProperty('mmo_life', usr.getProperty('mmo_life')-power);
					if(usr.getProperty('mmo_life') <= 0){
						this.pipe.sendRaw('mmo_player_kill', {target:target})
					}
				}else{
					Ape.log('Attacking unknow unit');
					return;
				}
			}
			this.pipe.sendRaw('mmo_firespell', {
				from: user.getProperty('pubid'),
				spell: spell,
				target: target,
				power: power
			});
		}
	},
	//Events/Commands functions
	cmdStart: function(params, infos){
		if(this.updateUser(infos.user, params)){
			params.pipe = {pubid:params.pipe}
			this.pipe.sendRaw('mmo_start', params, {from:infos.user.pipe});

		}
	},
	cmdStop: function(params, infos){
		if(this.updateUser(infos.user, params)){
			params.pipe = {pubid:params.pipe}
			this.pipe.sendRaw('mmo_stop', params, {from:infos.user.pipe});
		}
	},
	cmdMmoSpell: function(params, infos){
		if(this.updateUser(infos.user, params, true)){

			var now = new Date().getTime();
			var target = params.target;
			var spell = this.spells[params.spell];

			if(spell){

				if(now - infos.user.cooldown[params.spell] < spell.cooldown){
					this.sendError(infos.user, 'This spell is not ready yet !', {stopIncant:true});
				}else{
					if(infos.user.incant){
						Ape.clearTimeout(infos.user.incant);
					}
					//Ape.log('Ok '+params.spell+' '+now+'-'+infos.user.cooldown[params.spell]+'='+(now-infos.user.cooldown[params.spell])+'<'+spell.cooldown);
					infos.user.cooldown[params.spell] = now;

					if(spell.incant){
						var pipe = Ape.getPipe(params.pipe);

						pipe.sendRaw('mmo_incant', {'a':'b'}, {from:infos.user.pipe});

						infos.user.incant = Ape.setTimeout(this.endIncant.bind(this,[infos.user, params.spell, target]), spell.incant);
					}else{
						this.endIncant(infos.user, params.spell, target);
					}
				}
			}else{
				this.sendError(infos.user, 'Unknow spell', {stopIncant:true});
			}
		}

	},
	updateUser: function (user, params, doNotStopIncant){

		//User is not on the mmo channel
		if(!user.mmo_ok) return false;

		params.user = {pubid:user.getProperty('pubid')};

		if(params.pos && params.pos.x && params.pos.y){
			user.setProperty('posx', params.pos.x);
			user.setProperty('posy', params.pos.y);
		}
		if(params.dir){
			user.dir = params.dir;
		}
		if(user.incant && !doNotStopIncant){
			Ape.clearTimeout(user.incant);
		}
		return true;
	},
	mkchan: function(channel){
		if(channel.getProperty('name') == this.channel)
			this.pipe = channel.pipe;
	},
	beforeJoin: function(user, channel){
		if(channel.getProperty('name') == this.channel){
			user.mmo_ok = true;
			user.dir = '';
			user.cooldown = {};
			for(var i in this.spells){
				user.cooldown[i] = 0;
			}
			user.setProperty('mmo_avatar', $random(1,this.userTypeCount));
			user.setProperty('mmo_life', 1000);
		}
	},
	afterJoin: function(user, channel){
		if(channel.getProperty('name') == this.channel) {
			//Send monster list
			if(this.creeps.getLength() > 0) {
				this.updateCreeps();
				user.pipe.sendRaw('mmo_creeps', {creeps:this.creeps});
			}
		}
	}

});
new ApeMMO_Server();
