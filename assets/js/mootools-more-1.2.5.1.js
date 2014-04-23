//MooTools More, <http://mootools.net/more>. Copyright (c) 2006-2009 Aaron Newton <http://clientcide.com/>, Valerio Proietti <http://mad4milk.net> & the MooTools team <http://mootools.net/developers>, MIT Style License.

/*
---

script: More.js

name: More

description: MooTools More

license: MIT-style license

requires:
  - Core/MooTools

provides: [MooTools.More]

...
*/

MooTools.More = {
	'version': '1.2.5.1',
	'build': '254884f2b83651bf95260eed5c6cceb838e22d8e'
};


/*
---

script: MooTools.Lang.js

name: MooTools.Lang

description: Provides methods for localization.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Events
  - /MooTools.More

provides: [Lang]

...
*/

(function(){

	var data = {
		language: 'en-US',
		languages: {
			'en-US': {}
		},
		cascades: ['en-US']
	};

	var cascaded;

	MooTools.lang = new Events();

	$extend(MooTools.lang, {

		setLanguage: function(lang){
			if (!data.languages[lang]) return this;
			data.language = lang;
			this.load();
			this.fireEvent('langChange', lang);
			return this;
		},

		load: function() {
			var langs = this.cascade(this.getCurrentLanguage());
			cascaded = {};
			$each(langs, function(set, setName){
				cascaded[setName] = this.lambda(set);
			}, this);
		},

		getCurrentLanguage: function(){
			return data.language;
		},

		addLanguage: function(lang){
			data.languages[lang] = data.languages[lang] || {};
			return this;
		},

		cascade: function(lang){
			var cascades = (data.languages[lang] || {}).cascades || [];
			cascades.combine(data.cascades);
			cascades.erase(lang).push(lang);
			var langs = cascades.map(function(lng){
				return data.languages[lng];
			}, this);
			return $merge.apply(this, langs);
		},

		lambda: function(set) {
			(set || {}).get = function(key, args){
				return $lambda(set[key]).apply(this, $splat(args));
			};
			return set;
		},

		get: function(set, key, args){
			if (cascaded && cascaded[set]) return (key ? cascaded[set].get(key, args) : cascaded[set]);
		},

		set: function(lang, set, members){
			this.addLanguage(lang);
			langData = data.languages[lang];
			if (!langData[set]) langData[set] = {};
			$extend(langData[set], members);
			if (lang == this.getCurrentLanguage()){
				this.load();
				this.fireEvent('langChange', lang);
			}
			return this;
		},

		list: function(){
			return Hash.getKeys(data.languages);
		}

	});

})();


/*
---

script: Log.js

name: Log

description: Provides basic logging functionality for plugins to implement.

license: MIT-style license

authors:
  - Guillermo Rauch
  - Thomas Aylott
  - Scott Kyle

requires:
  - Core/Class
  - /MooTools.More

provides: [Log]

...
*/

(function(){

var global = this;

var log = function(){
	if (global.console && console.log){
		try {
			console.log.apply(console, arguments);
		} catch(e) {
			console.log(Array.slice(arguments));
		}
	} else {
		Log.logged.push(arguments);
	}
	return this;
};

var disabled = function(){
	this.logged.push(arguments);
	return this;
};

this.Log = new Class({

	logged: [],

	log: disabled,

	resetLog: function(){
		this.logged.empty();
		return this;
	},

	enableLog: function(){
		this.log = log;
		this.logged.each(function(args){
			this.log.apply(this, args);
		}, this);
		return this.resetLog();
	},

	disableLog: function(){
		this.log = disabled;
		return this;
	}

});

Log.extend(new Log).enableLog();

// legacy
Log.logger = function(){
	return this.log.apply(this, arguments);
};

})();


/*
---

script: Class.Refactor.js

name: Class.Refactor

description: Extends a class onto itself with new property, preserving any items attached to the class's namespace.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Class
  - /MooTools.More

# Some modules declare themselves dependent on Class.Refactor
provides: [Class.refactor, Class.Refactor]

...
*/

Class.refactor = function(original, refactors){

	$each(refactors, function(item, name){
		var origin = original.prototype[name];
		if (origin && (origin = origin._origin ? origin._origin: origin) && typeof item == 'function') original.implement(name, function(){
			var old = this.previous;
			this.previous = origin;
			var value = item.apply(this, arguments);
			this.previous = old;
			return value;
		}); else original.implement(name, item);
	});

	return original;

};


/*
---

script: Class.Binds.js

name: Class.Binds

description: Automagically binds specified methods in a class to the instance of the class.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Class
  - /MooTools.More

provides: [Class.Binds]

...
*/

Class.Mutators.Binds = function(binds){
    return binds;
};

Class.Mutators.initialize = function(initialize){
	return function(){
		$splat(this.Binds).each(function(name){
			var original = this[name];
			if (original) this[name] = original.bind(this);
		}, this);
		return initialize.apply(this, arguments);
	};
};


/*
---

script: Class.Occlude.js

name: Class.Occlude

description: Prevents a class from being applied to a DOM element twice.

license: MIT-style license.

authors:
  - Aaron Newton

requires:
  - Core/Class
  - Core/Element
  - /MooTools.More

provides: [Class.Occlude]

...
*/

Class.Occlude = new Class({

	occlude: function(property, element){
		element = document.id(element || this.element);
		var instance = element.retrieve(property || this.property);
		if (instance && !$defined(this.occluded))
			return this.occluded = instance;

		this.occluded = false;
		element.store(property || this.property, this);
		return this.occluded;
	}

});


/*
---

script: Date.js

name: Date

description: Extends the Date native object to include methods useful in managing dates.

license: MIT-style license

authors:
  - Aaron Newton
  - Nicholas Barthelemy - https://svn.nbarthelemy.com/date-js/
  - Harald Kirshner - mail [at] digitarald.de; http://digitarald.de
  - Scott Kyle - scott [at] appden.com; http://appden.com

requires:
  - Core/Array
  - Core/String
  - Core/Number
  - /Lang
  - /Date.English.US
  - /MooTools.More

provides: [Date]

...
*/

(function(){

var Date = this.Date;

if (!Date.now) Date.now = $time;

Date.Methods = {
	ms: 'Milliseconds',
	year: 'FullYear',
	min: 'Minutes',
	mo: 'Month',
	sec: 'Seconds',
	hr: 'Hours'
};

['Date', 'Day', 'FullYear', 'Hours', 'Milliseconds', 'Minutes', 'Month', 'Seconds', 'Time', 'TimezoneOffset',
	'Week', 'Timezone', 'GMTOffset', 'DayOfYear', 'LastMonth', 'LastDayOfMonth', 'UTCDate', 'UTCDay', 'UTCFullYear',
	'AMPM', 'Ordinal', 'UTCHours', 'UTCMilliseconds', 'UTCMinutes', 'UTCMonth', 'UTCSeconds', 'UTCMilliseconds'].each(function(method){
	Date.Methods[method.toLowerCase()] = method;
});

var pad = function(what, length){
	return new Array(length - String(what).length + 1).join('0') + what;
};

Date.implement({

	set: function(prop, value){
		switch ($type(prop)){
			case 'object':
				for (var p in prop) this.set(p, prop[p]);
				break;
			case 'string':
				prop = prop.toLowerCase();
				var m = Date.Methods;
				if (m[prop]) this['set' + m[prop]](value);
		}
		return this;
	},

	get: function(prop){
		prop = prop.toLowerCase();
		var m = Date.Methods;
		if (m[prop]) return this['get' + m[prop]]();
		return null;
	},

	clone: function(){
		return new Date(this.get('time'));
	},

	increment: function(interval, times){
		interval = interval || 'day';
		times = $pick(times, 1);

		switch (interval){
			case 'year':
				return this.increment('month', times * 12);
			case 'month':
				var d = this.get('date');
				this.set('date', 1).set('mo', this.get('mo') + times);
				return this.set('date', d.min(this.get('lastdayofmonth')));
			case 'week':
				return this.increment('day', times * 7);
			case 'day':
				return this.set('date', this.get('date') + times);
		}

		if (!Date.units[interval]) throw new Error(interval + ' is not a supported interval');

		return this.set('time', this.get('time') + times * Date.units[interval]());
	},

	decrement: function(interval, times){
		return this.increment(interval, -1 * $pick(times, 1));
	},

	isLeapYear: function(){
		return Date.isLeapYear(this.get('year'));
	},

	clearTime: function(){
		return this.set({hr: 0, min: 0, sec: 0, ms: 0});
	},

	diff: function(date, resolution){
		if ($type(date) == 'string') date = Date.parse(date);

		return ((date - this) / Date.units[resolution || 'day'](3, 3)).round(); // non-leap year, 30-day month
	},

	getLastDayOfMonth: function(){
		return Date.daysInMonth(this.get('mo'), this.get('year'));
	},

	getDayOfYear: function(){
		return (Date.UTC(this.get('year'), this.get('mo'), this.get('date') + 1)
			- Date.UTC(this.get('year'), 0, 1)) / Date.units.day();
	},

	getWeek: function(){
		return (this.get('dayofyear') / 7).ceil();
	},

	getOrdinal: function(day){
		return Date.getMsg('ordinal', day || this.get('date'));
	},

	getTimezone: function(){
		return this.toString()
			.replace(/^.*? ([A-Z]{3}).[0-9]{4}.*$/, '$1')
			.replace(/^.*?\(([A-Z])[a-z]+ ([A-Z])[a-z]+ ([A-Z])[a-z]+\)$/, '$1$2$3');
	},

	getGMTOffset: function(){
		var off = this.get('timezoneOffset');
		return ((off > 0) ? '-' : '+') + pad((off.abs() / 60).floor(), 2) + pad(off % 60, 2);
	},

	setAMPM: function(ampm){
		ampm = ampm.toUpperCase();
		var hr = this.get('hr');
		if (hr > 11 && ampm == 'AM') return this.decrement('hour', 12);
		else if (hr < 12 && ampm == 'PM') return this.increment('hour', 12);
		return this;
	},

	getAMPM: function(){
		return (this.get('hr') < 12) ? 'AM' : 'PM';
	},

	parse: function(str){
		this.set('time', Date.parse(str));
		return this;
	},

	isValid: function(date) {
		return !isNaN((date || this).valueOf());
	},

	format: function(f){
		if (!this.isValid()) return 'invalid date';
		f = f || '%x %X';
		f = formats[f.toLowerCase()] || f; // replace short-hand with actual format
		var d = this;
		return f.replace(/%([a-z%])/gi,
			function($0, $1){
				switch ($1){
					case 'a': return Date.getMsg('days')[d.get('day')].substr(0, 3);
					case 'A': return Date.getMsg('days')[d.get('day')];
					case 'b': return Date.getMsg('months')[d.get('month')].substr(0, 3);
					case 'B': return Date.getMsg('months')[d.get('month')];
					case 'c': return d.toString();
					case 'd': return pad(d.get('date'), 2);
					case 'D': return d.get('date');
					case 'e': return d.get('date');
					case 'H': return pad(d.get('hr'), 2);
					case 'I': return ((d.get('hr') % 12) || 12);
					case 'j': return pad(d.get('dayofyear'), 3);
					case 'm': return pad((d.get('mo') + 1), 2);
					case 'M': return pad(d.get('min'), 2);
					case 'o': return d.get('ordinal');
					case 'p': return Date.getMsg(d.get('ampm'));
					case 's': return Math.round(d / 1000);
					case 'S': return pad(d.get('seconds'), 2);
					case 'U': return pad(d.get('week'), 2);
					case 'w': return d.get('day');
					case 'x': return d.format(Date.getMsg('shortDate'));
					case 'X': return d.format(Date.getMsg('shortTime'));
					case 'y': return d.get('year').toString().substr(2);
					case 'Y': return d.get('year');
					case 'T': return d.get('GMTOffset');
					case 'Z': return d.get('Timezone');
					case 'z': return pad(d.get('ms'), 3);
				}
				return $1;
			}
		);
	},

	toISOString: function(){
		return this.format('iso8601');
	}

});

Date.alias('toISOString', 'toJSON');
Date.alias('diff', 'compare');
Date.alias('format', 'strftime');

var formats = {
	db: '%Y-%m-%d %H:%M:%S',
	compact: '%Y%m%dT%H%M%S',
	iso8601: '%Y-%m-%dT%H:%M:%S%T',
	rfc822: '%a, %d %b %Y %H:%M:%S %Z',
	'short': '%d %b %H:%M',
	'long': '%B %d, %Y %H:%M'
};

var parsePatterns = [];
var nativeParse = Date.parse;

var parseWord = function(type, word, num){
	var ret = -1;
	var translated = Date.getMsg(type + 's');
	switch ($type(word)){
		case 'object':
			ret = translated[word.get(type)];
			break;
		case 'number':
			ret = translated[word];
			if (!ret) throw new Error('Invalid ' + type + ' index: ' + word);
			break;
		case 'string':
			var match = translated.filter(function(name){
				return this.test(name);
			}, new RegExp('^' + word, 'i'));
			if (!match.length)    throw new Error('Invalid ' + type + ' string');
			if (match.length > 1) throw new Error('Ambiguous ' + type);
			ret = match[0];
	}

	return (num) ? translated.indexOf(ret) : ret;
};

Date.extend({

	getMsg: function(key, args) {
		return MooTools.lang.get('Date', key, args);
	},

	units: {
		ms: $lambda(1),
		second: $lambda(1000),
		minute: $lambda(60000),
		hour: $lambda(3600000),
		day: $lambda(86400000),
		week: $lambda(608400000),
		month: function(month, year){
			var d = new Date;
			return Date.daysInMonth($pick(month, d.get('mo')), $pick(year, d.get('year'))) * 86400000;
		},
		year: function(year){
			year = year || new Date().get('year');
			return Date.isLeapYear(year) ? 31622400000 : 31536000000;
		}
	},

	daysInMonth: function(month, year){
		return [31, Date.isLeapYear(year) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month];
	},

	isLeapYear: function(year){
		return ((year % 4 === 0) && (year % 100 !== 0)) || (year % 400 === 0);
	},

	parse: function(from){
		var t = $type(from);
		if (t == 'number') return new Date(from);
		if (t != 'string') return from;
		from = from.clean();
		if (!from.length) return null;

		var parsed;
		parsePatterns.some(function(pattern){
			var bits = pattern.re.exec(from);
			return (bits) ? (parsed = pattern.handler(bits)) : false;
		});

		return parsed || new Date(nativeParse(from));
	},

	parseDay: function(day, num){
		return parseWord('day', day, num);
	},

	parseMonth: function(month, num){
		return parseWord('month', month, num);
	},

	parseUTC: function(value){
		var localDate = new Date(value);
		var utcSeconds = Date.UTC(
			localDate.get('year'),
			localDate.get('mo'),
			localDate.get('date'),
			localDate.get('hr'),
			localDate.get('min'),
			localDate.get('sec'),
			localDate.get('ms')
		);
		return new Date(utcSeconds);
	},

	orderIndex: function(unit){
		return Date.getMsg('dateOrder').indexOf(unit) + 1;
	},

	defineFormat: function(name, format){
		formats[name] = format;
	},

	defineFormats: function(formats){
		for (var name in formats) Date.defineFormat(name, formats[name]);
	},

	parsePatterns: parsePatterns, // this is deprecated

	defineParser: function(pattern){
		parsePatterns.push((pattern.re && pattern.handler) ? pattern : build(pattern));
	},

	defineParsers: function(){
		Array.flatten(arguments).each(Date.defineParser);
	},

	define2DigitYearStart: function(year){
		startYear = year % 100;
		startCentury = year - startYear;
	}

});

var startCentury = 1900;
var startYear = 70;

var regexOf = function(type){
	return new RegExp('(?:' + Date.getMsg(type).map(function(name){
		return name.substr(0, 3);
	}).join('|') + ')[a-z]*');
};

var replacers = function(key){
	switch(key){
		case 'x': // iso8601 covers yyyy-mm-dd, so just check if month is first
			return ((Date.orderIndex('month') == 1) ? '%m[-./]%d' : '%d[-./]%m') + '([-./]%y)?';
		case 'X':
			return '%H([.:]%M)?([.:]%S([.:]%s)?)? ?%p? ?%T?';
	}
	return null;
};

var keys = {
	d: /[0-2]?[0-9]|3[01]/,
	H: /[01]?[0-9]|2[0-3]/,
	I: /0?[1-9]|1[0-2]/,
	M: /[0-5]?\d/,
	s: /\d+/,
	o: /[a-z]*/,
	p: /[ap]\.?m\.?/,
	y: /\d{2}|\d{4}/,
	Y: /\d{4}/,
	T: /Z|[+-]\d{2}(?::?\d{2})?/
};

keys.m = keys.I;
keys.S = keys.M;

var currentLanguage;

var recompile = function(language){
	currentLanguage = language;

	keys.a = keys.A = regexOf('days');
	keys.b = keys.B = regexOf('months');

	parsePatterns.each(function(pattern, i){
		if (pattern.format) parsePatterns[i] = build(pattern.format);
	});
};

var build = function(format){
	if (!currentLanguage) return {format: format};

	var parsed = [];
	var re = (format.source || format) // allow format to be regex
	 .replace(/%([a-z])/gi,
		function($0, $1){
			return replacers($1) || $0;
		}
	).replace(/\((?!\?)/g, '(?:') // make all groups non-capturing
	 .replace(/ (?!\?|\*)/g, ',? ') // be forgiving with spaces and commas
	 .replace(/%([a-z%])/gi,
		function($0, $1){
			var p = keys[$1];
			if (!p) return $1;
			parsed.push($1);
			return '(' + p.source + ')';
		}
	).replace(/\[a-z\]/gi, '[a-z\\u00c0-\\uffff]'); // handle unicode words

	return {
		format: format,
		re: new RegExp('^' + re + '$', 'i'),
		handler: function(bits){
			bits = bits.slice(1).associate(parsed);
			var date = new Date().clearTime(),
				year = bits.y || bits.Y;

			if (year != null) handle.call(date, 'y', year); // need to start in the right year
			if ('d' in bits) handle.call(date, 'd', 1);
			if ('m' in bits || 'b' in bits || 'B' in bits) handle.call(date, 'm', 1);

			for (var key in bits) handle.call(date, key, bits[key]);
			return date;
		}
	};
};

var handle = function(key, value){
	if (!value) return this;

	switch(key){
		case 'a': case 'A': return this.set('day', Date.parseDay(value, true));
		case 'b': case 'B': return this.set('mo', Date.parseMonth(value, true));
		case 'd': return this.set('date', value);
		case 'H': case 'I': return this.set('hr', value);
		case 'm': return this.set('mo', value - 1);
		case 'M': return this.set('min', value);
		case 'p': return this.set('ampm', value.replace(/\./g, ''));
		case 'S': return this.set('sec', value);
		case 's': return this.set('ms', ('0.' + value) * 1000);
		case 'w': return this.set('day', value);
		case 'Y': return this.set('year', value);
		case 'y':
			value = +value;
			if (value < 100) value += startCentury + (value < startYear ? 100 : 0);
			return this.set('year', value);
		case 'T':
			if (value == 'Z') value = '+00';
			var offset = value.match(/([+-])(\d{2}):?(\d{2})?/);
			offset = (offset[1] + '1') * (offset[2] * 60 + (+offset[3] || 0)) + this.getTimezoneOffset();
			return this.set('time', this - offset * 60000);
	}

	return this;
};

Date.defineParsers(
	'%Y([-./]%m([-./]%d((T| )%X)?)?)?', // "1999-12-31", "1999-12-31 11:59pm", "1999-12-31 23:59:59", ISO8601
	'%Y%m%d(T%H(%M%S?)?)?', // "19991231", "19991231T1159", compact
	'%x( %X)?', // "12/31", "12.31.99", "12-31-1999", "12/31/2008 11:59 PM"
	'%d%o( %b( %Y)?)?( %X)?', // "31st", "31st December", "31 Dec 1999", "31 Dec 1999 11:59pm"
	'%b( %d%o)?( %Y)?( %X)?', // Same as above with month and day switched
	'%Y %b( %d%o( %X)?)?', // Same as above with year coming first
	'%o %b %d %X %T %Y' // "Thu Oct 22 08:11:23 +0000 2009"
);

MooTools.lang.addEvent('langChange', function(language){
	if (MooTools.lang.get('Date')) recompile(language);
}).fireEvent('langChange', MooTools.lang.getCurrentLanguage());

})();


/*
---

script: Hash.Extras.js

name: Hash.Extras

description: Extends the Hash native object to include getFromPath which allows a path notation to child elements.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Hash.base
  - /MooTools.More

provides: [Hash.Extras]

...
*/

Hash.implement({

	getFromPath: function(notation){
		var source = this.getClean();
		notation.replace(/\[([^\]]+)\]|\.([^.[]+)|[^[.]+/g, function(match){
			if (!source) return null;
			var prop = arguments[2] || arguments[1] || arguments[0];
			source = (prop in source) ? source[prop] : null;
			return match;
		});
		return source;
	},

	cleanValues: function(method){
		method = method || $defined;
		this.each(function(v, k){
			if (!method(v)) this.erase(k);
		}, this);
		return this;
	},

	run: function(){
		var args = arguments;
		this.each(function(v, k){
			if ($type(v) == 'function') v.run(args);
		});
	}

});

/*
---

script: String.Extras.js

name: String.Extras

description: Extends the String native object to include methods useful in managing various kinds of strings (query strings, urls, html, etc).

license: MIT-style license

authors:
  - Aaron Newton
  - Guillermo Rauch
  - Christopher Pitt

requires:
  - Core/String
  - Core/$util
  - Core/Array

provides: [String.Extras]

...
*/

(function(){

var special = {
	'a': '[àáâãäåăą]',
	'A': '[ÀÁÂÃÄÅĂĄ]',
	'c': '[ćčç]',
	'C': '[ĆČÇ]',
	'd': '[ďđ]',
	'D': '[ĎÐ]',
	'e': '[èéêëěę]',
	'E': '[ÈÉÊËĚĘ]',
	'g': '[ğ]',
	'G': '[Ğ]',
	'i': '[ìíîï]',
	'I': '[ÌÍÎÏ]',
	'l': '[ĺľł]',
	'L': '[ĹĽŁ]',
	'n': '[ñňń]',
	'N': '[ÑŇŃ]',
	'o': '[òóôõöøő]',
	'O': '[ÒÓÔÕÖØ]',
	'r': '[řŕ]',
	'R': '[ŘŔ]',
	's': '[ššş]',
	'S': '[ŠŞŚ]',
	't': '[ťţ]',
	'T': '[ŤŢ]',
	'ue': '[ü]',
	'UE': '[Ü]',
	'u': '[ùúûůµ]',
	'U': '[ÙÚÛŮ]',
	'y': '[ÿý]',
	'Y': '[ŸÝ]',
	'z': '[žźż]',
	'Z': '[ŽŹŻ]',
	'th': '[þ]',
	'TH': '[Þ]',
	'dh': '[ð]',
	'DH': '[Ð]',
	'ss': '[ß]',
	'oe': '[œ]',
	'OE': '[Œ]',
	'ae': '[æ]',
	'AE': '[Æ]'
},

tidy = {
	' ': '[\xa0\u2002\u2003\u2009]',
	'*': '[\xb7]',
	'\'': '[\u2018\u2019]',
	'"': '[\u201c\u201d]',
	'...': '[\u2026]',
	'-': '[\u2013]',
	'--': '[\u2014]',
	'&raquo;': '[\uFFFD]'
};

function walk(string, replacements) {
	var result = string;

	for (key in replacements) {
		result = result.replace(new RegExp(replacements[key], 'g'), key);
	}

	return result;
}

function getRegForTag(tag, contents) {
	tag = tag || '';
	var regstr = contents ? "<" + tag + "(?!\\w)[^>]*>([\\s\\S]*?)<\/" + tag + "(?!\\w)>" : "<\/?" + tag + "([^>]+)?>";
	reg = new RegExp(regstr, "gi");
	return reg;
};

String.implement({

	standardize: function(){
		return walk(this, special);
	},

	repeat: function(times){
		return new Array(times + 1).join(this);
	},

	pad: function(length, str, dir){
		if (this.length >= length) return this;
		var pad = (str == null ? ' ' : '' + str).repeat(length - this.length).substr(0, length - this.length);
		if (!dir || dir == 'right') return this + pad;
		if (dir == 'left') return pad + this;
		return pad.substr(0, (pad.length / 2).floor()) + this + pad.substr(0, (pad.length / 2).ceil());
	},

	getTags: function(tag, contents){
		return this.match(getRegForTag(tag, contents)) || [];
	},

	stripTags: function(tag, contents){
		return this.replace(getRegForTag(tag, contents), '');
	},

	tidy: function(){
		return walk(this, tidy);
	}

});

})();


/*
---

script: String.QueryString.js

name: String.QueryString

description: Methods for dealing with URI query strings.

license: MIT-style license

authors:
  - Sebastian Markbåge, Aaron Newton, Lennart Pilon, Valerio Proietti

requires:
  - Core/Array
  - Core/String
  - /MooTools.More

provides: [String.QueryString]

...
*/

String.implement({

	parseQueryString: function(decodeKeys, decodeValues){
		if (decodeKeys == null) decodeKeys = true;
		if (decodeValues == null) decodeValues = true;
		var vars = this.split(/[&;]/), res = {};
		if (vars.length) vars.each(function(val){
			var index = val.indexOf('='),
				keys = index < 0 ? [''] : val.substr(0, index).match(/([^\]\[]+|(\B)(?=\]))/g),
				value = decodeValues ? decodeURIComponent(val.substr(index + 1)) : val.substr(index + 1),
				obj = res;
			keys.each(function(key, i){
				if (decodeKeys) key = decodeURIComponent(key);
				var current = obj[key];
				if(i < keys.length - 1)
					obj = obj[key] = current || {};
				else if($type(current) == 'array')
					current.push(value);
				else
					obj[key] = $defined(current) ? [current, value] : value;
			});
		});
		return res;
	},

	cleanQueryString: function(method){
		return this.split('&').filter(function(val){
			var index = val.indexOf('='),
			key = index < 0 ? '' : val.substr(0, index),
			value = val.substr(index + 1);
			return method ? method.run([key, value]) : $chk(value);
		}).join('&');
	}

});

/*
---

script: Element.Forms.js

name: Element.Forms

description: Extends the Element native object to include methods useful in managing inputs.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Element
  - /MooTools.More

provides: [Element.Forms]

...
*/

Element.implement({

	tidy: function(){
		this.set('value', this.get('value').tidy());
	},

	getTextInRange: function(start, end){
		return this.get('value').substring(start, end);
	},

	getSelectedText: function(){
		if (this.setSelectionRange) return this.getTextInRange(this.getSelectionStart(), this.getSelectionEnd());
		return document.selection.createRange().text;
	},

	getSelectedRange: function() {
		if ($defined(this.selectionStart)) return {start: this.selectionStart, end: this.selectionEnd};
		var pos = {start: 0, end: 0};
		var range = this.getDocument().selection.createRange();
		if (!range || range.parentElement() != this) return pos;
		var dup = range.duplicate();
		if (this.type == 'text') {
			pos.start = 0 - dup.moveStart('character', -100000);
			pos.end = pos.start + range.text.length;
		} else {
			var value = this.get('value');
			var offset = value.length;
			dup.moveToElementText(this);
			dup.setEndPoint('StartToEnd', range);
			if(dup.text.length) offset -= value.match(/[\n\r]*$/)[0].length;
			pos.end = offset - dup.text.length;
			dup.setEndPoint('StartToStart', range);
			pos.start = offset - dup.text.length;
		}
		return pos;
	},

	getSelectionStart: function(){
		return this.getSelectedRange().start;
	},

	getSelectionEnd: function(){
		return this.getSelectedRange().end;
	},

	setCaretPosition: function(pos){
		if (pos == 'end') pos = this.get('value').length;
		this.selectRange(pos, pos);
		return this;
	},

	getCaretPosition: function(){
		return this.getSelectedRange().start;
	},

	selectRange: function(start, end){
		if (this.setSelectionRange) {
			this.focus();
			this.setSelectionRange(start, end);
		} else {
			var value = this.get('value');
			var diff = value.substr(start, end - start).replace(/\r/g, '').length;
			start = value.substr(0, start).replace(/\r/g, '').length;
			var range = this.createTextRange();
			range.collapse(true);
			range.moveEnd('character', start + diff);
			range.moveStart('character', start);
			range.select();
		}
		return this;
	},

	insertAtCursor: function(value, select){
		var pos = this.getSelectedRange();
		var text = this.get('value');
		this.set('value', text.substring(0, pos.start) + value + text.substring(pos.end, text.length));
		if ($pick(select, true)) this.selectRange(pos.start, pos.start + value.length);
		else this.setCaretPosition(pos.start + value.length);
		return this;
	},

	insertAroundCursor: function(options, select){
		options = $extend({
			before: '',
			defaultMiddle: '',
			after: ''
		}, options);
		var value = this.getSelectedText() || options.defaultMiddle;
		var pos = this.getSelectedRange();
		var text = this.get('value');
		if (pos.start == pos.end){
			this.set('value', text.substring(0, pos.start) + options.before + value + options.after + text.substring(pos.end, text.length));
			this.selectRange(pos.start + options.before.length, pos.end + options.before.length + value.length);
		} else {
			var current = text.substring(pos.start, pos.end);
			this.set('value', text.substring(0, pos.start) + options.before + current + options.after + text.substring(pos.end, text.length));
			var selStart = pos.start + options.before.length;
			if ($pick(select, true)) this.selectRange(selStart, selStart + current.length);
			else this.setCaretPosition(selStart + text.length);
		}
		return this;
	}

});

/*
---

script: Elements.From.js

name: Elements.From

description: Returns a collection of elements from a string of html.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Element
  - /MooTools.More

provides: [Elements.from, Elements.From]

...
*/

Elements.from = function(text, excludeScripts){
	if ($pick(excludeScripts, true)) text = text.stripScripts();

	var container, match = text.match(/^\s*<(t[dhr]|tbody|tfoot|thead)/i);

	if (match){
		container = new Element('table');
		var tag = match[1].toLowerCase();
		if (['td', 'th', 'tr'].contains(tag)){
			container = new Element('tbody').inject(container);
			if (tag != 'tr') container = new Element('tr').inject(container);
		}
	}

	return (container || new Element('div')).set('html', text).getChildren();
};


/*
---

script: Element.Delegation.js

name: Element.Delegation

description: Extends the Element native object to include the delegate method for more efficient event management.

credits:
  - "Event checking based on the work of Daniel Steigerwald. License: MIT-style license.	Copyright: Copyright (c) 2008 Daniel Steigerwald, daniel.steigerwald.cz"

license: MIT-style license

authors:
  - Aaron Newton
  - Daniel Steigerwald

requires:
  - Core/Element.Event
  - Core/Selectors
  - /MooTools.More

provides: [Element.Delegation]

...
*/

(function(addEvent, removeEvent){

	var match = /(.*?):relay\(((?:\(.*?\)|.)+)\)$/,
		combinators = /[+>~\s]/,
		splitType = function(type){
			var bits = type.match(match);
			return !bits ? {event: type} : {
				event: bits[1],
				selector: bits[2]
			};
		},
		check = function(e, selector){
			var t = e.target;
			if (combinators.test(selector = selector.trim())){
				var els = this.getElements(selector);
				for (var i = els.length; i--; ){
					var el = els[i];
					if (t == el || el.hasChild(t)) return el;
				}
			} else {
				for ( ; t && t != this; t = t.parentNode){
					if (Element.match(t, selector)) return document.id(t);
				}
			}
			return null;
		};

	Element.implement({

		addEvent: function(type, fn){
			var split = splitType(type);
			if (split.selector){
				var monitors = this.retrieve('delegation:_delegateMonitors', {});
				if (!monitors[type]){
					var monitor = function(e){
						var el = check.call(this, e, split.selector);
						if (el) this.fireEvent(type, [e, el], 0, el);
					}.bind(this);
					monitors[type] = monitor;
					addEvent.call(this, split.event, monitor);
				}
			}
			return addEvent.apply(this, arguments);
		},

		removeEvent: function(type, fn){
			var split = splitType(type);
			if (split.selector){
				var events = this.retrieve('events');
				if (!events || !events[type] || (fn && !events[type].keys.contains(fn))) return this;

				if (fn) removeEvent.apply(this, [type, fn]);
				else removeEvent.apply(this, type);

				events = this.retrieve('events');
				if (events && events[type] && events[type].keys.length == 0){
					var monitors = this.retrieve('delegation:_delegateMonitors', {});
					removeEvent.apply(this, [split.event, monitors[type]]);
					delete monitors[type];
				}
				return this;
			}
			return removeEvent.apply(this, arguments);
		},

		fireEvent: function(type, args, delay, bind){
			var events = this.retrieve('events');
			var e, el;
			if (args) {
				e = args[0];
				el = args[1];
			}
			if (!events || !events[type]) return this;
			events[type].keys.each(function(fn){
				fn.create({bind: bind || this, delay: delay, arguments: args})();
			}, this);
			return this;
		}

	});

})(Element.prototype.addEvent, Element.prototype.removeEvent);

try {
	if (typeof HTMLElement != 'undefined')
		HTMLElement.prototype.fireEvent = Element.prototype.fireEvent;
} catch(e){}


/*
---

script: Element.Measure.js

name: Element.Measure

description: Extends the Element native object to include methods useful in measuring dimensions.

credits: "Element.measure / .expose methods by Daniel Steigerwald License: MIT-style license. Copyright: Copyright (c) 2008 Daniel Steigerwald, daniel.steigerwald.cz"

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Element.Style
  - Core/Element.Dimensions
  - /MooTools.More

provides: [Element.Measure]

...
*/

Element.implement({

	measure: function(fn){
		var vis = function(el) {
			return !!(!el || el.offsetHeight || el.offsetWidth);
		};
		if (vis(this)) return fn.apply(this);
		var parent = this.getParent(),
			restorers = [],
			toMeasure = [];
		while (!vis(parent) && parent != document.body) {
			toMeasure.push(parent.expose());
			parent = parent.getParent();
		}
		var restore = this.expose();
		var result = fn.apply(this);
		restore();
		toMeasure.each(function(restore){
			restore();
		});
		return result;
	},

	expose: function(){
		if (this.getStyle('display') != 'none') return $empty;
		var before = this.style.cssText;
		this.setStyles({
			display: 'block',
			position: 'absolute',
			visibility: 'hidden'
		});
		return function(){
			this.style.cssText = before;
		}.bind(this);
	},

	getDimensions: function(options){
		options = $merge({computeSize: false},options);
		var dim = {};
		var getSize = function(el, options){
			return (options.computeSize)?el.getComputedSize(options):el.getSize();
		};
		var parent = this.getParent('body');
		if (parent && this.getStyle('display') == 'none'){
			dim = this.measure(function(){
				return getSize(this, options);
			});
		} else if (parent){
			try { //safari sometimes crashes here, so catch it
				dim = getSize(this, options);
			}catch(e){}
		} else {
			dim = {x: 0, y: 0};
		}
		return $chk(dim.x) ? $extend(dim, {width: dim.x, height: dim.y}) : $extend(dim, {x: dim.width, y: dim.height});
	},

	getComputedSize: function(options){
		//legacy support for my stupid spelling error
		if (options && options.plains) options.planes = options.plains;

		options = $merge({
			styles: ['padding','border'],
			planes: {
				height: ['top','bottom'],
				width: ['left','right']
			},
			mode: 'both'
		}, options);

		var size = {width: 0,height: 0};
		switch (options.mode){
			case 'vertical':
				delete size.width;
				delete options.planes.width;
				break;
			case 'horizontal':
				delete size.height;
				delete options.planes.height;
				break;
		}
		var getStyles = [];
		//this function might be useful in other places; perhaps it should be outside this function?
		$each(options.planes, function(plane, key){
			plane.each(function(edge){
				options.styles.each(function(style){
					getStyles.push((style == 'border') ? style + '-' + edge + '-' + 'width' : style + '-' + edge);
				});
			});
		});
		var styles = {};
		getStyles.each(function(style){ styles[style] = this.getComputedStyle(style); }, this);
		var subtracted = [];
		$each(options.planes, function(plane, key){ //keys: width, height, planes: ['left', 'right'], ['top','bottom']
			var capitalized = key.capitalize();
			size['total' + capitalized] = size['computed' + capitalized] = 0;
			plane.each(function(edge){ //top, left, right, bottom
				size['computed' + edge.capitalize()] = 0;
				getStyles.each(function(style, i){ //padding, border, etc.
					//'padding-left'.test('left') size['totalWidth'] = size['width'] + [padding-left]
					if (style.test(edge)){
						styles[style] = styles[style].toInt() || 0; //styles['padding-left'] = 5;
						size['total' + capitalized] = size['total' + capitalized] + styles[style];
						size['computed' + edge.capitalize()] = size['computed' + edge.capitalize()] + styles[style];
					}
					//if width != width (so, padding-left, for instance), then subtract that from the total
					if (style.test(edge) && key != style &&
						(style.test('border') || style.test('padding')) && !subtracted.contains(style)){
						subtracted.push(style);
						size['computed' + capitalized] = size['computed' + capitalized]-styles[style];
					}
				});
			});
		});

		['Width', 'Height'].each(function(value){
			var lower = value.toLowerCase();
			if(!$chk(size[lower])) return;

			size[lower] = size[lower] + this['offset' + value] + size['computed' + value];
			size['total' + value] = size[lower] + size['total' + value];
			delete size['computed' + value];
		}, this);

		return $extend(styles, size);
	}

});

/*
---

script: Element.Position.js

name: Element.Position

description: Extends the Element native object to include methods useful positioning elements relative to others.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Element.Dimensions
  - /Element.Measure

provides: [Element.Position]

...
*/

(function(){

var original = Element.prototype.position;

Element.implement({

	position: function(options){
		//call original position if the options are x/y values
		if (options && ($defined(options.x) || $defined(options.y))) return original ? original.apply(this, arguments) : this;
		$each(options||{}, function(v, k){ if (!$defined(v)) delete options[k]; });
		options = $merge({
			// minimum: { x: 0, y: 0 },
			// maximum: { x: 0, y: 0},
			relativeTo: document.body,
			position: {
				x: 'center', //left, center, right
				y: 'center' //top, center, bottom
			},
			edge: false,
			offset: {x: 0, y: 0},
			returnPos: false,
			relFixedPosition: false,
			ignoreMargins: false,
			ignoreScroll: false,
			allowNegative: false
		}, options);
		//compute the offset of the parent positioned element if this element is in one
		var parentOffset = {x: 0, y: 0},
				parentPositioned = false;
		/* dollar around getOffsetParent should not be necessary, but as it does not return
		 * a mootools extended element in IE, an error occurs on the call to expose. See:
		 * http://mootools.lighthouseapp.com/projects/2706/tickets/333-element-getoffsetparent-inconsistency-between-ie-and-other-browsers */
		var offsetParent = this.measure(function(){
			return document.id(this.getOffsetParent());
		});
		if (offsetParent && offsetParent != this.getDocument().body){
			parentOffset = offsetParent.measure(function(){
				return this.getPosition();
			});
			parentPositioned = offsetParent != document.id(options.relativeTo);
			options.offset.x = options.offset.x - parentOffset.x;
			options.offset.y = options.offset.y - parentOffset.y;
		}
		//upperRight, bottomRight, centerRight, upperLeft, bottomLeft, centerLeft
		//topRight, topLeft, centerTop, centerBottom, center
		var fixValue = function(option){
			if ($type(option) != 'string') return option;
			option = option.toLowerCase();
			var val = {};

			if (option.test('left')) val.x = 'left';
			else if (option.test('right')) val.x = 'right';
			else val.x = 'center';

			if (option.test('upper') || option.test('top')) val.y = 'top';
			else if (option.test('bottom')) val.y = 'bottom';
			else val.y = 'center';

			return val;
		};
		options.edge = fixValue(options.edge);
		options.position = fixValue(options.position);
		if (!options.edge){
			if (options.position.x == 'center' && options.position.y == 'center') options.edge = {x:'center', y:'center'};
			else options.edge = {x:'left', y:'top'};
		}

		this.setStyle('position', 'absolute');
		var rel = document.id(options.relativeTo) || document.body,
				calc = rel == document.body ? window.getScroll() : rel.getPosition(),
				top = calc.y, left = calc.x;

		var dim = this.getDimensions({computeSize: true, styles:['padding', 'border','margin']});
		var pos = {},
				prefY = options.offset.y,
				prefX = options.offset.x,
				winSize = window.getSize();
		switch(options.position.x){
			case 'left':
				pos.x = left + prefX;
				break;
			case 'right':
				pos.x = left + prefX + rel.offsetWidth;
				break;
			default: //center
				pos.x = left + ((rel == document.body ? winSize.x : rel.offsetWidth)/2) + prefX;
				break;
		}
		switch(options.position.y){
			case 'top':
				pos.y = top + prefY;
				break;
			case 'bottom':
				pos.y = top + prefY + rel.offsetHeight;
				break;
			default: //center
				pos.y = top + ((rel == document.body ? winSize.y : rel.offsetHeight)/2) + prefY;
				break;
		}
		if (options.edge){
			var edgeOffset = {};

			switch(options.edge.x){
				case 'left':
					edgeOffset.x = 0;
					break;
				case 'right':
					edgeOffset.x = -dim.x-dim.computedRight-dim.computedLeft;
					break;
				default: //center
					edgeOffset.x = -(dim.totalWidth/2);
					break;
			}
			switch(options.edge.y){
				case 'top':
					edgeOffset.y = 0;
					break;
				case 'bottom':
					edgeOffset.y = -dim.y-dim.computedTop-dim.computedBottom;
					break;
				default: //center
					edgeOffset.y = -(dim.totalHeight/2);
					break;
			}
			pos.x += edgeOffset.x;
			pos.y += edgeOffset.y;
		}
		pos = {
			left: ((pos.x >= 0 || parentPositioned || options.allowNegative) ? pos.x : 0).toInt(),
			top: ((pos.y >= 0 || parentPositioned || options.allowNegative) ? pos.y : 0).toInt()
		};
		var xy = {left: 'x', top: 'y'};
		['minimum', 'maximum'].each(function(minmax) {
			['left', 'top'].each(function(lr) {
				var val = options[minmax] ? options[minmax][xy[lr]] : null;
				if (val != null && ((minmax == 'minimum') ? pos[lr] < val: pos[lr] > val)) pos[lr] = val;
			});
		});
		if (rel.getStyle('position') == 'fixed' || options.relFixedPosition){
			var winScroll = window.getScroll();
			pos.top+= winScroll.y;
			pos.left+= winScroll.x;
		}
		var relScroll = rel.getScroll();
		if (options.ignoreScroll) {
			pos.top -= relScroll.y;
			pos.left -= relScroll.x;
		} else {
			pos.top += relScroll.y;
			pos.left += relScroll.x;
		}
		if (options.ignoreMargins) {
			pos.left += (
				options.edge.x == 'right' ? dim['margin-right'] :
				options.edge.x == 'center' ? -dim['margin-left'] + ((dim['margin-right'] + dim['margin-left'])/2) :
					- dim['margin-left']
			);
			pos.top += (
				options.edge.y == 'bottom' ? dim['margin-bottom'] :
				options.edge.y == 'center' ? -dim['margin-top'] + ((dim['margin-bottom'] + dim['margin-top'])/2) :
					- dim['margin-top']
			);
		}
		pos.left = Math.ceil(pos.left);
		pos.top = Math.ceil(pos.top);
		if (options.returnPos) return pos;
		else this.setStyles(pos);
		return this;
	}

});

})();


/*
---

script: Element.Shortcuts.js

name: Element.Shortcuts

description: Extends the Element native object to include some shortcut methods.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Element.Style
  - /MooTools.More

provides: [Element.Shortcuts]

...
*/

Element.implement({

	isDisplayed: function(){
		return this.getStyle('display') != 'none';
	},

	isVisible: function(){
		var w = this.offsetWidth,
			h = this.offsetHeight;
		return (w == 0 && h == 0) ? false : (w > 0 && h > 0) ? true : this.style.display != 'none';
	},

	toggle: function(){
		return this[this.isDisplayed() ? 'hide' : 'show']();
	},

	hide: function(){
		var d;
		try {
			//IE fails here if the element is not in the dom
			d = this.getStyle('display');
		} catch(e){}
		if (d == "none") return this;
		return this.store('element:_originalDisplay', d || '').setStyle('display', 'none');
	},

	show: function(display){
		if (!display && this.isDisplayed()) return this;
		display = display || this.retrieve('element:_originalDisplay') || 'block';
		return this.setStyle('display', (display == 'none') ? 'block' : display);
	},

	swapClass: function(remove, add){
		return this.removeClass(remove).addClass(add);
	}
});

Document.implement({
	clearSelection: function(){
		if (document.selection && document.selection.empty) {
			document.selection.empty();
		} else if (window.getSelection) {
			var selection = window.getSelection();
			if (selection && selection.removeAllRanges) selection.removeAllRanges();
		}
	}
});

/*
---

script: Form.Request.js

name: Form.Request

description: Handles the basic functionality of submitting a form and updating a dom element with the result.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Request.HTML
  - /Class.Binds
  - /Class.Occlude
  - /Spinner
  - /String.QueryString
  - /Element.Delegation

provides: [Form.Request]

...
*/

if (!window.Form) window.Form = {};

(function(){

	Form.Request = new Class({

		Binds: ['onSubmit', 'onFormValidate'],

		Implements: [Options, Events, Class.Occlude],

		options: {
			//onFailure: $empty,
			//onSuccess: #empty, //aliased to onComplete,
			//onSend: $empty
			requestOptions: {
				evalScripts: true,
				useSpinner: true,
				emulation: false,
				link: 'ignore'
			},
			sendButtonClicked: true,
			extraData: {},
			resetForm: true
		},

		property: 'form.request',

		initialize: function(form, update, options) {
			this.element = document.id(form);
			if (this.occlude()) return this.occluded;
			this.update = document.id(update);
			this.setOptions(options);
			this.makeRequest();
			if (this.options.resetForm) {
				this.request.addEvent('success', function(){
					$try(function(){ this.element.reset(); }.bind(this));
					if (window.OverText) OverText.update();
				}.bind(this));
			}
			this.attach();
		},

		toElement: function() {
			return this.element;
		},

		makeRequest: function(){
			this.request = new Request.HTML($merge({
					update: this.update,
					emulation: false,
					spinnerTarget: this.element,
					method: this.element.get('method') || 'post'
			}, this.options.requestOptions)).addEvents({
				success: function(tree, elements, html, javascript){
					['complete', 'success'].each(function(evt){
						this.fireEvent(evt, [this.update, tree, elements, html, javascript]);
					}, this);
				}.bind(this),
				failure: function(){
					this.fireEvent('complete', arguments).fireEvent('failure', arguments);
				}.bind(this),
				exception: function(){
					this.fireEvent('failure', arguments);
				}.bind(this)
			});
		},

		attach: function(attach){
			attach = $pick(attach, true);
			method = attach ? 'addEvent' : 'removeEvent';

			this.element[method]('click:relay(button, input[type=submit])', this.saveClickedButton.bind(this));

			var fv = this.element.retrieve('validator');
			if (fv) fv[method]('onFormValidate', this.onFormValidate);
			else this.element[method]('submit', this.onSubmit);
		},

		detach: function(){
			this.attach(false);
			return this;
		},

		//public method
		enable: function(){
			this.attach();
			return this;
		},

		//public method
		disable: function(){
			this.detach();
			return this;
		},

		onFormValidate: function(valid, form, e) {
			//if there's no event, then this wasn't a submit event
			if (!e) return;
			var fv = this.element.retrieve('validator');
			if (valid || (fv && !fv.options.stopOnFailure)) {
				if (e && e.stop) e.stop();
				this.send();
			}
		},

		onSubmit: function(e){
			var fv = this.element.retrieve('validator');
			if (fv) {
				//form validator was created after Form.Request
				this.element.removeEvent('submit', this.onSubmit);
				fv.addEvent('onFormValidate', this.onFormValidate);
				this.element.validate();
				return;
			}
			if (e) e.stop();
			this.send();
		},

		saveClickedButton: function(event, target) {
			if (!this.options.sendButtonClicked) return;
			if (!target.get('name')) return;
			this.options.extraData[target.get('name')] = target.get('value') || true;
			this.clickedCleaner = function(){
				delete this.options.extraData[target.get('name')];
				this.clickedCleaner = $empty;
			}.bind(this);
		},

		clickedCleaner: $empty,

		send: function(){
			var str = this.element.toQueryString().trim();
			var data = $H(this.options.extraData).toQueryString();
			if (str) str += "&" + data;
			else str = data;
			this.fireEvent('send', [this.element, str.parseQueryString()]);
			this.request.send({data: str, url: this.element.get("action")});
			this.clickedCleaner();
			return this;
		}

	});

	Element.Properties.formRequest = {

		set: function(){
			var opt = Array.link(arguments, {options: Object.type, update: Element.type, updateId: String.type});
			var update = opt.update || opt.updateId;
			var updater = this.retrieve('form.request');
			if (update) {
				if (updater) updater.update = document.id(update);
				this.store('form.request:update', update);
			}
			if (opt.options) {
				if (updater) updater.setOptions(opt.options);
				this.store('form.request:options', opt.options);
			}
			return this;
		},

		get: function(){
			var opt = Array.link(arguments, {options: Object.type, update: Element.type, updateId: String.type});
			var update = opt.update || opt.updateId;
			if (opt.options || update || !this.retrieve('form.request')){
				if (opt.options || !this.retrieve('form.request:options')) this.set('form.request', opt.options);
				if (update) this.set('form.request', update);
				this.store('form.request', new Form.Request(this, this.retrieve('form.request:update'), this.retrieve('form.request:options')));
			}
			return this.retrieve('form.request');
		}

	};

	Element.implement({

		formUpdate: function(update, options){
			this.get('formRequest', update, options).send();
			return this;
		}

	});

})();

/*
---

script: Form.Request.Append.js

name: Form.Request.Append

description: Handles the basic functionality of submitting a form and updating a dom element with the result. The result is appended to the DOM element instead of replacing its contents.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - /Form.Request
  - /Fx.Reveal
  - /Elements.from

provides: [Form.Request.Append]

...
*/

Form.Request.Append = new Class({

	Extends: Form.Request,

	options: {
		//onBeforeEffect: $empty,
		useReveal: true,
		revealOptions: {},
		inject: 'bottom'
	},

	makeRequest: function(){
		this.request = new Request.HTML($merge({
				url: this.element.get('action'),
				method: this.element.get('method') || 'post',
				spinnerTarget: this.element
			}, this.options.requestOptions, {
				evalScripts: false
			})
		).addEvents({
			success: function(tree, elements, html, javascript){
				var container;
				var kids = Elements.from(html);
				if (kids.length == 1) {
					container = kids[0];
				} else {
					 container = new Element('div', {
						styles: {
							display: 'none'
						}
					}).adopt(kids);
				}
				container.inject(this.update, this.options.inject);
				if (this.options.requestOptions.evalScripts) $exec(javascript);
				this.fireEvent('beforeEffect', container);
				var finish = function(){
					this.fireEvent('success', [container, this.update, tree, elements, html, javascript]);
				}.bind(this);
				if (this.options.useReveal) {
					container.get('reveal', this.options.revealOptions).chain(finish);
					container.reveal();
				} else {
					finish();
				}
			}.bind(this),
			failure: function(xhr){
				this.fireEvent('failure', xhr);
			}.bind(this)
		});
	}

});

/*
---

script: Form.Validator.js

name: Form.Validator

description: A css-class based form validation system.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Options
  - Core/Events
  - Core/Selectors
  - Core/Element.Event
  - Core/Element.Style
  - Core/JSON
  - /Lang
  - /Class.Binds
  - /Date
  - /Element.Forms
  - /Form.Validator.English
  - /Element.Shortcuts

provides: [Form.Validator, InputValidator, FormValidator.BaseValidators]

...
*/
if (!window.Form) window.Form = {};

var InputValidator = new Class({

	Implements: [Options],

	options: {
		errorMsg: 'Validation failed.',
		test: function(field){return true;}
	},

	initialize: function(className, options){
		this.setOptions(options);
		this.className = className;
	},

	test: function(field, props){
		if (document.id(field)) return this.options.test(document.id(field), props||this.getProps(field));
		else return false;
	},

	getError: function(field, props){
		var err = this.options.errorMsg;
		if ($type(err) == 'function') err = err(document.id(field), props||this.getProps(field));
		return err;
	},

	getProps: function(field){
		if (!document.id(field)) return {};
		return field.get('validatorProps');
	}

});

Element.Properties.validatorProps = {

	set: function(props){
		return this.eliminate('validatorProps').store('validatorProps', props);
	},

	get: function(props){
		if (props) this.set(props);
		if (this.retrieve('validatorProps')) return this.retrieve('validatorProps');
		if (this.getProperty('validatorProps')){
			try {
				this.store('validatorProps', JSON.decode(this.getProperty('validatorProps')));
			}catch(e){
				return {};
			}
		} else {
			var vals = this.get('class').split(' ').filter(function(cls){
				return cls.test(':');
			});
			if (!vals.length){
				this.store('validatorProps', {});
			} else {
				props = {};
				vals.each(function(cls){
					var split = cls.split(':');
					if (split[1]) {
						try {
							props[split[0]] = JSON.decode(split[1]);
						} catch(e) {}
					}
				});
				this.store('validatorProps', props);
			}
		}
		return this.retrieve('validatorProps');
	}

};

Form.Validator = new Class({

	Implements:[Options, Events],

	Binds: ['onSubmit'],

	options: {/*
		onFormValidate: $empty(isValid, form, event),
		onElementValidate: $empty(isValid, field, className, warn),
		onElementPass: $empty(field),
		onElementFail: $empty(field, validatorsFailed) */
		fieldSelectors: 'input, select, textarea',
		ignoreHidden: true,
		ignoreDisabled: true,
		useTitles: false,
		evaluateOnSubmit: true,
		evaluateFieldsOnBlur: true,
		evaluateFieldsOnChange: true,
		serial: true,
		stopOnFailure: true,
		warningPrefix: function(){
			return Form.Validator.getMsg('warningPrefix') || 'Warning: ';
		},
		errorPrefix: function(){
			return Form.Validator.getMsg('errorPrefix') || 'Error: ';
		}
	},

	initialize: function(form, options){
		this.setOptions(options);
		this.element = document.id(form);
		this.element.store('validator', this);
		this.warningPrefix = $lambda(this.options.warningPrefix)();
		this.errorPrefix = $lambda(this.options.errorPrefix)();
		if (this.options.evaluateOnSubmit) this.element.addEvent('submit', this.onSubmit);
		if (this.options.evaluateFieldsOnBlur || this.options.evaluateFieldsOnChange) this.watchFields(this.getFields());
	},

	toElement: function(){
		return this.element;
	},

	getFields: function(){
		return (this.fields = this.element.getElements(this.options.fieldSelectors));
	},

	watchFields: function(fields){
		fields.each(function(el){
			if (this.options.evaluateFieldsOnBlur)
				el.addEvent('blur', this.validationMonitor.pass([el, false], this));
			if (this.options.evaluateFieldsOnChange)
				el.addEvent('change', this.validationMonitor.pass([el, true], this));
		}, this);
	},

	validationMonitor: function(){
		$clear(this.timer);
		this.timer = this.validateField.delay(50, this, arguments);
	},

	onSubmit: function(event){
		if (!this.validate(event) && event) event.preventDefault();
		else this.reset();
	},

	reset: function(){
		this.getFields().each(this.resetField, this);
		return this;
	},

	validate: function(event){
		var result = this.getFields().map(function(field){
			return this.validateField(field, true);
		}, this).every(function(v){ return v;});
		this.fireEvent('formValidate', [result, this.element, event]);
		if (this.options.stopOnFailure && !result && event) event.preventDefault();
		return result;
	},

	validateField: function(field, force){
		if (this.paused) return true;
		field = document.id(field);
		var passed = !field.hasClass('validation-failed');
		var failed, warned;
		if (this.options.serial && !force){
			failed = this.element.getElement('.validation-failed');
			warned = this.element.getElement('.warning');
		}
		if (field && (!failed || force || field.hasClass('validation-failed') || (failed && !this.options.serial))){
			var validators = field.className.split(' ').some(function(cn){
				return this.getValidator(cn);
			}, this);
			var validatorsFailed = [];
			field.className.split(' ').each(function(className){
				if (className && !this.test(className, field)) validatorsFailed.include(className);
			}, this);
			passed = validatorsFailed.length === 0;
			if (validators && !field.hasClass('warnOnly')){
				if (passed){
					field.addClass('validation-passed').removeClass('validation-failed');
					this.fireEvent('elementPass', field);
				} else {
					field.addClass('validation-failed').removeClass('validation-passed');
					this.fireEvent('elementFail', [field, validatorsFailed]);
				}
			}
			if (!warned){
				var warnings = field.className.split(' ').some(function(cn){
					if (cn.test('^warn-') || field.hasClass('warnOnly'))
						return this.getValidator(cn.replace(/^warn-/,''));
					else return null;
				}, this);
				field.removeClass('warning');
				var warnResult = field.className.split(' ').map(function(cn){
					if (cn.test('^warn-') || field.hasClass('warnOnly'))
						return this.test(cn.replace(/^warn-/,''), field, true);
					else return null;
				}, this);
			}
		}
		return passed;
	},

	test: function(className, field, warn){
		field = document.id(field);
		if((this.options.ignoreHidden && !field.isVisible()) || (this.options.ignoreDisabled && field.get('disabled'))) return true;
		var validator = this.getValidator(className);
		warn = $pick(warn, false);
		if (field.hasClass('warnOnly')) warn = true;
		var isValid = field.hasClass('ignoreValidation') || (validator ? validator.test(field) : true);
		if (validator && field.isVisible()) this.fireEvent('elementValidate', [isValid, field, className, warn]);
		if (warn) return true;
		return isValid;
	},

	resetField: function(field){
		field = document.id(field);
		if (field){
			field.className.split(' ').each(function(className){
				if (className.test('^warn-')) className = className.replace(/^warn-/, '');
				field.removeClass('validation-failed');
				field.removeClass('warning');
				field.removeClass('validation-passed');
			}, this);
		}
		return this;
	},

	stop: function(){
		this.paused = true;
		return this;
	},

	start: function(){
		this.paused = false;
		return this;
	},

	ignoreField: function(field, warn){
		field = document.id(field);
		if (field){
			this.enforceField(field);
			if (warn) field.addClass('warnOnly');
			else field.addClass('ignoreValidation');
		}
		return this;
	},

	enforceField: function(field){
		field = document.id(field);
		if (field) field.removeClass('warnOnly').removeClass('ignoreValidation');
		return this;
	}

});

Form.Validator.getMsg = function(key){
	return MooTools.lang.get('Form.Validator', key);
};

Form.Validator.adders = {

	validators:{},

	add : function(className, options){
		this.validators[className] = new InputValidator(className, options);
		//if this is a class (this method is used by instances of Form.Validator and the Form.Validator namespace)
		//extend these validators into it
		//this allows validators to be global and/or per instance
		if (!this.initialize){
			this.implement({
				validators: this.validators
			});
		}
	},

	addAllThese : function(validators){
		$A(validators).each(function(validator){
			this.add(validator[0], validator[1]);
		}, this);
	},

	getValidator: function(className){
		return this.validators[className.split(':')[0]];
	}

};

$extend(Form.Validator, Form.Validator.adders);

Form.Validator.implement(Form.Validator.adders);

Form.Validator.add('IsEmpty', {

	errorMsg: false,
	test: function(element){
		if (element.type == 'select-one' || element.type == 'select')
			return !(element.selectedIndex >= 0 && element.options[element.selectedIndex].value != '');
		else
			return ((element.get('value') == null) || (element.get('value').length == 0));
	}

});

Form.Validator.addAllThese([

	['required', {
		errorMsg: function(){
			return Form.Validator.getMsg('required');
		},
		test: function(element){
			return !Form.Validator.getValidator('IsEmpty').test(element);
		}
	}],

	['minLength', {
		errorMsg: function(element, props){
			if ($type(props.minLength))
				return Form.Validator.getMsg('minLength').substitute({minLength:props.minLength,length:element.get('value').length });
			else return '';
		},
		test: function(element, props){
			if ($type(props.minLength)) return (element.get('value').length >= $pick(props.minLength, 0));
			else return true;
		}
	}],

	['maxLength', {
		errorMsg: function(element, props){
			//props is {maxLength:10}
			if ($type(props.maxLength))
				return Form.Validator.getMsg('maxLength').substitute({maxLength:props.maxLength,length:element.get('value').length });
			else return '';
		},
		test: function(element, props){
			//if the value is <= than the maxLength value, element passes test
			return (element.get('value').length <= $pick(props.maxLength, 10000));
		}
	}],

	['validate-integer', {
		errorMsg: Form.Validator.getMsg.pass('integer'),
		test: function(element){
			return Form.Validator.getValidator('IsEmpty').test(element) || (/^(-?[1-9]\d*|0)$/).test(element.get('value'));
		}
	}],

	['validate-numeric', {
		errorMsg: Form.Validator.getMsg.pass('numeric'),
		test: function(element){
			return Form.Validator.getValidator('IsEmpty').test(element) ||
				(/^-?(?:0$0(?=\d*\.)|[1-9]|0)\d*(\.\d+)?$/).test(element.get('value'));
		}
	}],

	['validate-digits', {
		errorMsg: Form.Validator.getMsg.pass('digits'),
		test: function(element){
			return Form.Validator.getValidator('IsEmpty').test(element) || (/^[\d() .:\-\+#]+$/.test(element.get('value')));
		}
	}],

	['validate-alpha', {
		errorMsg: Form.Validator.getMsg.pass('alpha'),
		test: function(element){
			return Form.Validator.getValidator('IsEmpty').test(element) ||  (/^[a-zA-Z]+$/).test(element.get('value'));
		}
	}],

	['validate-alphanum', {
		errorMsg: Form.Validator.getMsg.pass('alphanum'),
		test: function(element){
			return Form.Validator.getValidator('IsEmpty').test(element) || !(/\W/).test(element.get('value'));
		}
	}],

	['validate-date', {
		errorMsg: function(element, props){
			if (Date.parse){
				var format = props.dateFormat || '%x';
				return Form.Validator.getMsg('dateSuchAs').substitute({date: new Date().format(format)});
			} else {
				return Form.Validator.getMsg('dateInFormatMDY');
			}
		},
		test: function(element, props){
			if (Form.Validator.getValidator('IsEmpty').test(element)) return true;
			var d;
			if (Date.parse){
				var format = props.dateFormat || '%x';
				d = Date.parse(element.get('value'));
				var formatted = d.format(format);
				if (formatted != 'invalid date') element.set('value', formatted);
				return !isNaN(d);
			} else {
				var regex = /^(\d{2})\/(\d{2})\/(\d{4})$/;
				if (!regex.test(element.get('value'))) return false;
				d = new Date(element.get('value').replace(regex, '$1/$2/$3'));
				return (parseInt(RegExp.$1, 10) == (1 + d.getMonth())) &&
					(parseInt(RegExp.$2, 10) == d.getDate()) &&
					(parseInt(RegExp.$3, 10) == d.getFullYear());
			}
		}
	}],

	['validate-email', {
		errorMsg: Form.Validator.getMsg.pass('email'),
		test: function(element){
			return Form.Validator.getValidator('IsEmpty').test(element) || (/^[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,4}$/i).test(element.get('value'));
		}
	}],

	['validate-url', {
		errorMsg: Form.Validator.getMsg.pass('url'),
		test: function(element){
			return Form.Validator.getValidator('IsEmpty').test(element) || (/^(https?|ftp|rmtp|mms):\/\/(([A-Z0-9][A-Z0-9_-]*)(\.[A-Z0-9][A-Z0-9_-]*)+)(:(\d+))?\/?/i).test(element.get('value'));
		}
	}],

	['validate-currency-dollar', {
		errorMsg: Form.Validator.getMsg.pass('currencyDollar'),
		test: function(element){
			// [$]1[##][,###]+[.##]
			// [$]1###+[.##]
			// [$]0.##
			// [$].##
			return Form.Validator.getValidator('IsEmpty').test(element) ||  (/^\$?\-?([1-9]{1}[0-9]{0,2}(\,[0-9]{3})*(\.[0-9]{0,2})?|[1-9]{1}\d*(\.[0-9]{0,2})?|0(\.[0-9]{0,2})?|(\.[0-9]{1,2})?)$/).test(element.get('value'));
		}
	}],

	['validate-one-required', {
		errorMsg: Form.Validator.getMsg.pass('oneRequired'),
		test: function(element, props){
			var p = document.id(props['validate-one-required']) || element.getParent(props['validate-one-required']);
			return p.getElements('input').some(function(el){
				if (['checkbox', 'radio'].contains(el.get('type'))) return el.get('checked');
				return el.get('value');
			});
		}
	}]

]);

Element.Properties.validator = {

	set: function(options){
		var validator = this.retrieve('validator');
		if (validator) validator.setOptions(options);
		return this.store('validator:options', options);
	},

	get: function(options){
		if (options || !this.retrieve('validator')){
			if (options || !this.retrieve('validator:options')) this.set('validator', options);
			this.store('validator', new Form.Validator(this, this.retrieve('validator:options')));
		}
		return this.retrieve('validator');
	}

};

Element.implement({

	validate: function(options){
		if (options) this.set('validator', options);
		return this.get('validator', options).validate();
	}

});
//legacy
var FormValidator = Form.Validator;


/*
---

script: Form.Validator.Inline.js

name: Form.Validator.Inline

description: Extends Form.Validator to add inline messages.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - /Form.Validator

provides: [Form.Validator.Inline]

...
*/

Form.Validator.Inline = new Class({

	Extends: Form.Validator,

	options: {
		showError: function(errorElement){
			if (errorElement.reveal) errorElement.reveal();
			else errorElement.setStyle('display', 'block');
		},
		hideError: function(errorElement){
			if (errorElement.dissolve) errorElement.dissolve();
			else errorElement.setStyle('display', 'none');
		},
		scrollToErrorsOnSubmit: true,
		scrollToErrorsOnBlur: false,
		scrollToErrorsOnChange: false,
		scrollFxOptions: {
			transition: 'quad:out',
			offset: {
				y: -20
			}
		}
	},

	initialize: function(form, options){
		this.parent(form, options);
		this.addEvent('onElementValidate', function(isValid, field, className, warn){
			var validator = this.getValidator(className);
			if (!isValid && validator.getError(field)){
				if (warn) field.addClass('warning');
				var advice = this.makeAdvice(className, field, validator.getError(field), warn);
				this.insertAdvice(advice, field);
				this.showAdvice(className, field);
			} else {
				this.hideAdvice(className, field);
			}
		});
	},

	makeAdvice: function(className, field, error, warn){
		var errorMsg = (warn)?this.warningPrefix:this.errorPrefix;
			errorMsg += (this.options.useTitles) ? field.title || error:error;
		var cssClass = (warn) ? 'warning-advice' : 'validation-advice';
		var advice = this.getAdvice(className, field);
		if(advice) {
			advice = advice.set('html', errorMsg);
		} else {
			advice = new Element('div', {
				html: errorMsg,
				styles: { display: 'none' },
				id: 'advice-' + className.split(':')[0] + '-' + this.getFieldId(field)
			}).addClass(cssClass);
		}
		field.store('advice-' + className, advice);
		return advice;
	},

	getFieldId : function(field){
		return field.id ? field.id : field.id = 'input_' + field.name;
	},

	showAdvice: function(className, field){
		var advice = this.getAdvice(className, field);
		if (advice && !field.retrieve(this.getPropName(className))
				&& (advice.getStyle('display') == 'none'
				|| advice.getStyle('visiblity') == 'hidden'
				|| advice.getStyle('opacity') == 0)){
			field.store(this.getPropName(className), true);
			this.options.showError(advice);
			this.fireEvent('showAdvice', [field, advice, className]);
		}
	},

	hideAdvice: function(className, field){
		var advice = this.getAdvice(className, field);
		if (advice && field.retrieve(this.getPropName(className))){
			field.store(this.getPropName(className), false);
			this.options.hideError(advice);
			this.fireEvent('hideAdvice', [field, advice, className]);
		}
	},

	getPropName: function(className){
		return 'advice' + className;
	},

	resetField: function(field){
		field = document.id(field);
		if (!field) return this;
		this.parent(field);
		field.className.split(' ').each(function(className){
			this.hideAdvice(className, field);
		}, this);
		return this;
	},

	getAllAdviceMessages: function(field, force){
		var advice = [];
		if (field.hasClass('ignoreValidation') && !force) return advice;
		var validators = field.className.split(' ').some(function(cn){
			var warner = cn.test('^warn-') || field.hasClass('warnOnly');
			if (warner) cn = cn.replace(/^warn-/, '');
			var validator = this.getValidator(cn);
			if (!validator) return;
			advice.push({
				message: validator.getError(field),
				warnOnly: warner,
				passed: validator.test(),
				validator: validator
			});
		}, this);
		return advice;
	},

	getAdvice: function(className, field){
		return field.retrieve('advice-' + className);
	},

	insertAdvice: function(advice, field){
		//Check for error position prop
		var props = field.get('validatorProps');
		//Build advice
		if (!props.msgPos || !document.id(props.msgPos)){
			if(field.type.toLowerCase() == 'radio') field.getParent().adopt(advice);
			else advice.inject(document.id(field), 'after');
		} else {
			document.id(props.msgPos).grab(advice);
		}
	},

	validateField: function(field, force, scroll){
		var result = this.parent(field, force);
		if (((this.options.scrollToErrorsOnSubmit && scroll === undefined) || scroll) && !result){
			var failed = document.id(this).getElement('.validation-failed');
			var par = document.id(this).getParent();
			while (par != document.body && par.getScrollSize().y == par.getSize().y){
				par = par.getParent();
			}
			var fx = par.retrieve('fvScroller');
			if (!fx && window.Fx && Fx.Scroll){
				fx = new Fx.Scroll(par, this.options.scrollFxOptions);
				par.store('fvScroller', fx);
			}
			if (failed){
				if (fx) fx.toElement(failed);
				else par.scrollTo(par.getScroll().x, failed.getPosition(par).y - 20);
			}
		}
		return result;
	},

	watchFields: function(fields){
		fields.each(function(el){
			if (this.options.evaluateFieldsOnBlur){
				el.addEvent('blur', this.validationMonitor.pass([el, false, this.options.scrollToErrorsOnBlur], this));
			}
			if (this.options.evaluateFieldsOnChange){
				el.addEvent('change', this.validationMonitor.pass([el, true, this.options.scrollToErrorsOnChange], this));
			}
		}, this);
	}

});


/*
---

script: Form.Validator.Extras.js

name: Form.Validator.Extras

description: Additional validators for the Form.Validator class.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - /Form.Validator

provides: [Form.Validator.Extras]

...
*/
Form.Validator.addAllThese([

	['validate-enforce-oncheck', {
		test: function(element, props){
			var fv = element.getParent('form').retrieve('validator');
			if (!fv) return true;
			(props.toEnforce || document.id(props.enforceChildrenOf).getElements('input, select, textarea')).map(function(item){
				if (element.checked) {
					fv.enforceField(item);
				} else {
					fv.ignoreField(item);
					fv.resetField(item);
				}
			});
			return true;
		}
	}],

	['validate-ignore-oncheck', {
		test: function(element, props){
			var fv = element.getParent('form').retrieve('validator');
			if (!fv) return true;
			(props.toIgnore || document.id(props.ignoreChildrenOf).getElements('input, select, textarea')).each(function(item){
				if (element.checked) {
					fv.ignoreField(item);
					fv.resetField(item);
				} else {
					fv.enforceField(item);
				}
			});
			return true;
		}
	}],

	['validate-nospace', {
		errorMsg: function(){
			return Form.Validator.getMsg('noSpace');
		},
		test: function(element, props){
			return !element.get('value').test(/\s/);
		}
	}],

	['validate-toggle-oncheck', {
		test: function(element, props){
			var fv = element.getParent('form').retrieve('validator');
			if (!fv) return true;
			var eleArr = props.toToggle || document.id(props.toToggleChildrenOf).getElements('input, select, textarea');
			if (!element.checked){
				eleArr.each(function(item){
					fv.ignoreField(item);
					fv.resetField(item);
				});
			} else {
				eleArr.each(function(item){
					fv.enforceField(item);
				});
			}
			return true;
		}
	}],

	['validate-reqchk-bynode', {
		errorMsg: function(){
			return Form.Validator.getMsg('reqChkByNode');
		},
		test: function(element, props){
			return (document.id(props.nodeId).getElements(props.selector || 'input[type=checkbox], input[type=radio]')).some(function(item){
				return item.checked;
			});
		}
	}],

	['validate-required-check', {
		errorMsg: function(element, props){
			return props.useTitle ? element.get('title') : Form.Validator.getMsg('requiredChk');
		},
		test: function(element, props){
			return !!element.checked;
		}
	}],

	['validate-reqchk-byname', {
		errorMsg: function(element, props){
			return Form.Validator.getMsg('reqChkByName').substitute({label: props.label || element.get('type')});
		},
		test: function(element, props){
			var grpName = props.groupName || element.get('name');
			var oneCheckedItem = $$(document.getElementsByName(grpName)).some(function(item, index){
				return item.checked;
			});
			var fv = element.getParent('form').retrieve('validator');
			if (oneCheckedItem && fv) fv.resetField(element);
			return oneCheckedItem;
		}
	}],

	['validate-match', {
		errorMsg: function(element, props){
			return Form.Validator.getMsg('match').substitute({matchName: props.matchName || document.id(props.matchInput).get('name')});
		},
		test: function(element, props){
			var eleVal = element.get('value');
			var matchVal = document.id(props.matchInput) && document.id(props.matchInput).get('value');
			return eleVal && matchVal ? eleVal == matchVal : true;
		}
	}],

	['validate-after-date', {
		errorMsg: function(element, props){
			return Form.Validator.getMsg('afterDate').substitute({
				label: props.afterLabel || (props.afterElement ? Form.Validator.getMsg('startDate') : Form.Validator.getMsg('currentDate'))
			});
		},
		test: function(element, props){
			var start = document.id(props.afterElement) ? Date.parse(document.id(props.afterElement).get('value')) : new Date();
			var end = Date.parse(element.get('value'));
			return end && start ? end >= start : true;
		}
	}],

	['validate-before-date', {
		errorMsg: function(element, props){
			return Form.Validator.getMsg('beforeDate').substitute({
				label: props.beforeLabel || (props.beforeElement ? Form.Validator.getMsg('endDate') : Form.Validator.getMsg('currentDate'))
			});
		},
		test: function(element, props){
			var start = Date.parse(element.get('value'));
			var end = document.id(props.beforeElement) ? Date.parse(document.id(props.beforeElement).get('value')) : new Date();
			return end && start ? end >= start : true;
		}
	}],

	['validate-custom-required', {
		errorMsg: function(){
			return Form.Validator.getMsg('required');
		},
		test: function(element, props){
			return element.get('value') != props.emptyValue;
		}
	}],

	['validate-same-month', {
		errorMsg: function(element, props){
			var startMo = document.id(props.sameMonthAs) && document.id(props.sameMonthAs).get('value');
			var eleVal = element.get('value');
			if (eleVal != '') return Form.Validator.getMsg(startMo ? 'sameMonth' : 'startMonth');
		},
		test: function(element, props){
			var d1 = Date.parse(element.get('value'));
			var d2 = Date.parse(document.id(props.sameMonthAs) && document.id(props.sameMonthAs).get('value'));
			return d1 && d2 ? d1.format('%B') == d2.format('%B') : true;
		}
	}],


	['validate-cc-num', {
		errorMsg: function(element){
			var ccNum = element.get('value').replace(/[^0-9]/g, '');
			return Form.Validator.getMsg('creditcard').substitute({length: ccNum.length});
		},
		test: function(element){
			// required is a different test
			if (Form.Validator.getValidator('IsEmpty').test(element)) { return true; }

			// Clean number value
			var ccNum = element.get('value');
			ccNum = ccNum.replace(/[^0-9]/g, '');

			var valid_type = false;

			if (ccNum.test(/^4[0-9]{12}([0-9]{3})?$/)) valid_type = 'Visa';
			else if (ccNum.test(/^5[1-5]([0-9]{14})$/)) valid_type = 'Master Card';
			else if (ccNum.test(/^3[47][0-9]{13}$/)) valid_type = 'American Express';
			else if (ccNum.test(/^6011[0-9]{12}$/)) valid_type = 'Discover';

			if (valid_type) {
				var sum = 0;
				var cur = 0;

				for(var i=ccNum.length-1; i>=0; --i) {
					cur = ccNum.charAt(i).toInt();
					if (cur == 0) { continue; }

					if ((ccNum.length-i) % 2 == 0) { cur += cur; }
					if (cur > 9) { cur = cur.toString().charAt(0).toInt() + cur.toString().charAt(1).toInt(); }

					sum += cur;
				}
				if ((sum % 10) == 0) { return true; }
			}

			var chunks = '';
			while (ccNum != '') {
				chunks += ' ' + ccNum.substr(0,4);
				ccNum = ccNum.substr(4);
			}

			element.getParent('form').retrieve('validator').ignoreField(element);
			element.set('value', chunks.clean());
			element.getParent('form').retrieve('validator').enforceField(element);
			return false;
		}
	}]


]);

/*
---

script: OverText.js

name: OverText

description: Shows text over an input that disappears when the user clicks into it. The text remains hidden if the user adds a value.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Options
  - Core/Events
  - Core/Element.Event
  - /Class.Binds
  - /Class.Occlude
  - /Element.Position
  - /Element.Shortcuts

provides: [OverText]

...
*/

var OverText = new Class({

	Implements: [Options, Events, Class.Occlude],

	Binds: ['reposition', 'assert', 'focus', 'hide'],

	options: {/*
		textOverride: null,
		onFocus: $empty()
		onTextHide: $empty(textEl, inputEl),
		onTextShow: $empty(textEl, inputEl), */
		element: 'label',
		positionOptions: {
			position: 'upperLeft',
			edge: 'upperLeft',
			offset: {
				x: 4,
				y: 2
			}
		},
		poll: false,
		pollInterval: 250,
		wrap: false
	},

	property: 'OverText',

	initialize: function(element, options){
		this.element = document.id(element);
		if (this.occlude()) return this.occluded;
		this.setOptions(options);
		this.attach(this.element);
		OverText.instances.push(this);
		if (this.options.poll) this.poll();
		return this;
	},

	toElement: function(){
		return this.element;
	},

	attach: function(){
		var val = this.options.textOverride || this.element.get('alt') || this.element.get('title');
		if (!val) return;
		this.text = new Element(this.options.element, {
			'class': 'overTxtLabel',
			styles: {
				lineHeight: 'normal',
				position: 'absolute',
				cursor: 'text'
			},
			html: val,
			events: {
				click: this.hide.pass(this.options.element == 'label', this)
			}
		}).inject(this.element, 'after');
		if (this.options.element == 'label') {
			if (!this.element.get('id')) this.element.set('id', 'input_' + new Date().getTime());
			this.text.set('for', this.element.get('id'));
		}

		if (this.options.wrap) {
			this.textHolder = new Element('div', {
				styles: {
					lineHeight: 'normal',
					position: 'relative'
				},
				'class':'overTxtWrapper'
			}).adopt(this.text).inject(this.element, 'before');
		}

		return this.enable();
	},

	destroy: function(){
		this.element.eliminate('OverTextDiv').eliminate('OverText');
		this.disable();
		if (this.text) this.text.destroy();
		if (this.textHolder) this.textHolder.destroy();
		return this;
	},

	disable: function(){
		this.element.removeEvents({
			focus: this.focus,
			blur: this.assert,
			change: this.assert
		});
		window.removeEvent('resize', this.reposition);
		this.hide(true, true);
		return this;
	},

	enable: function(){
		this.element.addEvents({
			focus: this.focus,
			blur: this.assert,
			change: this.assert
		});
		window.addEvent('resize', this.reposition);
		this.assert(true);
		this.reposition();
		return this;
	},

	wrap: function(){
		if (this.options.element == 'label') {
			if (!this.element.get('id')) this.element.set('id', 'input_' + new Date().getTime());
			this.text.set('for', this.element.get('id'));
		}
	},

	startPolling: function(){
		this.pollingPaused = false;
		return this.poll();
	},

	poll: function(stop){
		//start immediately
		//pause on focus
		//resumeon blur
		if (this.poller && !stop) return this;
		var test = function(){
			if (!this.pollingPaused) this.assert(true);
		}.bind(this);
		if (stop) $clear(this.poller);
		else this.poller = test.periodical(this.options.pollInterval, this);
		return this;
	},

	stopPolling: function(){
		this.pollingPaused = true;
		return this.poll(true);
	},

	focus: function(){
		if (this.text && (!this.text.isDisplayed() || this.element.get('disabled'))) return;
		this.hide();
	},

	hide: function(suppressFocus, force){
		if (this.text && (this.text.isDisplayed() && (!this.element.get('disabled') || force))){
			this.text.hide();
			this.fireEvent('textHide', [this.text, this.element]);
			this.pollingPaused = true;
			if (!suppressFocus){
				try {
					this.element.fireEvent('focus');
					this.element.focus();
				} catch(e){} //IE barfs if you call focus on hidden elements
			}
		}
		return this;
	},

	show: function(){
		if (this.text && !this.text.isDisplayed()){
			this.text.show();
			this.reposition();
			this.fireEvent('textShow', [this.text, this.element]);
			this.pollingPaused = false;
		}
		return this;
	},

	assert: function(suppressFocus){
		this[this.test() ? 'show' : 'hide'](suppressFocus);
	},

	test: function(){
		var v = this.element.get('value');
		return !v;
	},

	reposition: function(){
		this.assert(true);
		if (!this.element.isVisible()) return this.stopPolling().hide();
		if (this.text && this.test()) this.text.position($merge(this.options.positionOptions, {relativeTo: this.element}));
		return this;
	}

});

OverText.instances = [];

$extend(OverText, {

	each: function(fn) {
		return OverText.instances.map(function(ot, i){
			if (ot.element && ot.text) return fn.apply(OverText, [ot, i]);
			return null; //the input or the text was destroyed
		});
	},

	update: function(){

		return OverText.each(function(ot){
			return ot.reposition();
		});

	},

	hideAll: function(){

		return OverText.each(function(ot){
			return ot.hide(true, true);
		});

	},

	showAll: function(){
		return OverText.each(function(ot) {
			return ot.show();
		});
	}

});

if (window.Fx && Fx.Reveal) {
	Fx.Reveal.implement({
		hideInputs: Browser.Engine.trident ? 'select, input, textarea, object, embed, .overTxtLabel' : false
	});
}

/*
---

script: Fx.Reveal.js

name: Fx.Reveal

description: Defines Fx.Reveal, a class that shows and hides elements with a transition.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Fx.Morph
  - /Element.Shortcuts
  - /Element.Measure

provides: [Fx.Reveal]

...
*/

Fx.Reveal = new Class({

	Extends: Fx.Morph,

	options: {/*
		onShow: $empty(thisElement),
		onHide: $empty(thisElement),
		onComplete: $empty(thisElement),
		heightOverride: null,
		widthOverride: null, */
		link: 'cancel',
		styles: ['padding', 'border', 'margin'],
		transitionOpacity: !Browser.Engine.trident4,
		mode: 'vertical',
		display: function(){
			return this.element.get('tag') != 'tr' ? 'block' : 'table-row';
		},
		hideInputs: Browser.Engine.trident ? 'select, input, textarea, object, embed' : false,
		opacity: 1
	},

	dissolve: function(){
		try {
			if (!this.hiding && !this.showing){
				if (this.element.getStyle('display') != 'none'){
					this.hiding = true;
					this.showing = false;
					this.hidden = true;
					this.cssText = this.element.style.cssText;
					var startStyles = this.element.getComputedSize({
						styles: this.options.styles,
						mode: this.options.mode
					});
					this.element.setStyle('display', $lambda(this.options.display).apply(this));
					if (this.options.transitionOpacity) startStyles.opacity = this.options.opacity;
					var zero = {};
					$each(startStyles, function(style, name){
						zero[name] = [style, 0];
					}, this);
					this.element.setStyle('overflow', 'hidden');
					var hideThese = this.options.hideInputs ? this.element.getElements(this.options.hideInputs) : null;
					this.$chain.unshift(function(){
						if (this.hidden){
							this.hiding = false;
							$each(startStyles, function(style, name){
								startStyles[name] = style;
							}, this);
							this.element.style.cssText = this.cssText;
							this.element.setStyle('display', 'none');
							if (hideThese) hideThese.setStyle('visibility', 'visible');
						}
						this.fireEvent('hide', this.element);
						this.callChain();
					}.bind(this));
					if (hideThese) hideThese.setStyle('visibility', 'hidden');
					this.start(zero);
				} else {
					this.callChain.delay(10, this);
					this.fireEvent('complete', this.element);
					this.fireEvent('hide', this.element);
				}
			} else if (this.options.link == 'chain'){
				this.chain(this.dissolve.bind(this));
			} else if (this.options.link == 'cancel' && !this.hiding){
				this.cancel();
				this.dissolve();
			}
		} catch(e){
			this.hiding = false;
			this.element.setStyle('display', 'none');
			this.callChain.delay(10, this);
			this.fireEvent('complete', this.element);
			this.fireEvent('hide', this.element);
		}
		return this;
	},

	reveal: function(){
		try {
			if (!this.showing && !this.hiding){
				if (this.element.getStyle('display') == 'none'){
					this.showing = true;
					this.hiding = this.hidden =  false;
					var startStyles;
					this.cssText = this.element.style.cssText;
					//toggle display, but hide it
					this.element.measure(function(){
						//create the styles for the opened/visible state
						startStyles = this.element.getComputedSize({
							styles: this.options.styles,
							mode: this.options.mode
						});
					}.bind(this));
					$each(startStyles, function(style, name){
						startStyles[name] = style;
					});
					//if we're overridding height/width
					if ($chk(this.options.heightOverride)) startStyles.height = this.options.heightOverride.toInt();
					if ($chk(this.options.widthOverride)) startStyles.width = this.options.widthOverride.toInt();
					if (this.options.transitionOpacity) {
						this.element.setStyle('opacity', 0);
						startStyles.opacity = this.options.opacity;
					}
					//create the zero state for the beginning of the transition
					var zero = {
						height: 0,
						display: $lambda(this.options.display).apply(this)
					};
					$each(startStyles, function(style, name){ zero[name] = 0; });
					//set to zero
					this.element.setStyles($merge(zero, {overflow: 'hidden'}));
					//hide inputs
					var hideThese = this.options.hideInputs ? this.element.getElements(this.options.hideInputs) : null;
					if (hideThese) hideThese.setStyle('visibility', 'hidden');
					//start the effect
					this.start(startStyles);
					this.$chain.unshift(function(){
						this.element.style.cssText = this.cssText;
						this.element.setStyle('display', $lambda(this.options.display).apply(this));
						if (!this.hidden) this.showing = false;
						if (hideThese) hideThese.setStyle('visibility', 'visible');
						this.callChain();
						this.fireEvent('show', this.element);
					}.bind(this));
				} else {
					this.callChain();
					this.fireEvent('complete', this.element);
					this.fireEvent('show', this.element);
				}
			} else if (this.options.link == 'chain'){
				this.chain(this.reveal.bind(this));
			} else if (this.options.link == 'cancel' && !this.showing){
				this.cancel();
				this.reveal();
			}
		} catch(e){
			this.element.setStyles({
				display: $lambda(this.options.display).apply(this),
				visiblity: 'visible',
				opacity: this.options.opacity
			});
			this.showing = false;
			this.callChain.delay(10, this);
			this.fireEvent('complete', this.element);
			this.fireEvent('show', this.element);
		}
		return this;
	},

	toggle: function(){
		if (this.element.getStyle('display') == 'none'){
			this.reveal();
		} else {
			this.dissolve();
		}
		return this;
	},

	cancel: function(){
		this.parent.apply(this, arguments);
		this.element.style.cssText = this.cssText;
		this.hiding = false;
		this.showing = false;
		return this;
	}

});

Element.Properties.reveal = {

	set: function(options){
		var reveal = this.retrieve('reveal');
		if (reveal) reveal.cancel();
		return this.eliminate('reveal').store('reveal:options', options);
	},

	get: function(options){
		if (options || !this.retrieve('reveal')){
			if (options || !this.retrieve('reveal:options')) this.set('reveal', options);
			this.store('reveal', new Fx.Reveal(this, this.retrieve('reveal:options')));
		}
		return this.retrieve('reveal');
	}

};

Element.Properties.dissolve = Element.Properties.reveal;

Element.implement({

	reveal: function(options){
		this.get('reveal', options).reveal();
		return this;
	},

	dissolve: function(options){
		this.get('reveal', options).dissolve();
		return this;
	},

	nix: function(){
		var params = Array.link(arguments, {destroy: Boolean.type, options: Object.type});
		this.get('reveal', params.options).dissolve().chain(function(){
			this[params.destroy ? 'destroy' : 'dispose']();
		}.bind(this));
		return this;
	},

	wink: function(){
		var params = Array.link(arguments, {duration: Number.type, options: Object.type});
		var reveal = this.get('reveal', params.options);
		reveal.reveal().chain(function(){
			(function(){
				reveal.dissolve();
			}).delay(params.duration || 2000);
		});
	}


});

/*
---

script: Drag.js

name: Drag

description: The base Drag Class. Can be used to drag and resize Elements using mouse events.

license: MIT-style license

authors:
  - Valerio Proietti
  - Tom Occhinno
  - Jan Kassens

requires:
  - Core/Events
  - Core/Options
  - Core/Element.Event
  - Core/Element.Style
  - Core/Element.Dimensions
  - /MooTools.More

provides: [Drag]
...

*/

var Drag = new Class({

	Implements: [Events, Options],

	options: {/*
		onBeforeStart: $empty(thisElement),
		onStart: $empty(thisElement, event),
		onSnap: $empty(thisElement)
		onDrag: $empty(thisElement, event),
		onCancel: $empty(thisElement),
		onComplete: $empty(thisElement, event),*/
		snap: 6,
		unit: 'px',
		grid: false,
		style: true,
		limit: false,
		handle: false,
		invert: false,
		preventDefault: false,
		stopPropagation: false,
		modifiers: {x: 'left', y: 'top'}
	},

	initialize: function(){
		var params = Array.link(arguments, {'options': Object.type, 'element': $defined});
		this.element = document.id(params.element);
		this.document = this.element.getDocument();
		this.setOptions(params.options || {});
		var htype = $type(this.options.handle);
		this.handles = ((htype == 'array' || htype == 'collection') ? $$(this.options.handle) : document.id(this.options.handle)) || this.element;
		this.mouse = {'now': {}, 'pos': {}};
		this.value = {'start': {}, 'now': {}};

		this.selection = (Browser.Engine.trident) ? 'selectstart' : 'mousedown';

		this.bound = {
			start: this.start.bind(this),
			check: this.check.bind(this),
			drag: this.drag.bind(this),
			stop: this.stop.bind(this),
			cancel: this.cancel.bind(this),
			eventStop: $lambda(false)
		};
		this.attach();
	},

	attach: function(){
		this.handles.addEvent('mousedown', this.bound.start);
		return this;
	},

	detach: function(){
		this.handles.removeEvent('mousedown', this.bound.start);
		return this;
	},

	start: function(event){
		if (event.rightClick) return;
		if (this.options.preventDefault) event.preventDefault();
		if (this.options.stopPropagation) event.stopPropagation();
		this.mouse.start = event.page;
		this.fireEvent('beforeStart', this.element);
		var limit = this.options.limit;
		this.limit = {x: [], y: []};
		var styles = this.element.getStyles('left', 'right', 'top', 'bottom');
		this._invert = {
			x: this.options.modifiers.x == 'left' && styles.left == 'auto' &&
			   !isNaN(styles.right.toInt()) && (this.options.modifiers.x = 'right'),
			y: this.options.modifiers.y == 'top' && styles.top == 'auto' &&
			   !isNaN(styles.bottom.toInt()) && (this.options.modifiers.y = 'bottom')
		};

		var z, coordinates;
		for (z in this.options.modifiers){
			if (!this.options.modifiers[z]) continue;

			var style = this.element.getStyle(this.options.modifiers[z]);

			// Some browsers (IE and Opera) don't always return pixels.
			if (style && !style.match(/px$/)){
				if (!coordinates) coordinates = this.element.getCoordinates(this.element.getOffsetParent());
				style = coordinates[this.options.modifiers[z]];
			}

			if (this.options.style) this.value.now[z] = (style || 0).toInt();
			else this.value.now[z] = this.element[this.options.modifiers[z]];

			if (this.options.invert) this.value.now[z] *= -1;
			if (this._invert[z]) this.value.now[z] *= -1;

			this.mouse.pos[z] = event.page[z] - this.value.now[z];

			if (limit && limit[z]){
				for (var i = 2; i--; i){
					if ($chk(limit[z][i])) this.limit[z][i] = $lambda(limit[z][i])();
				}
			}
		}

		if ($type(this.options.grid) == 'number') this.options.grid = {x: this.options.grid, y: this.options.grid};
		this.document.addEvents({mousemove: this.bound.check, mouseup: this.bound.cancel});
		this.document.addEvent(this.selection, this.bound.eventStop);
	},

	check: function(event){
		if (this.options.preventDefault) event.preventDefault();
		var distance = Math.round(Math.sqrt(Math.pow(event.page.x - this.mouse.start.x, 2) + Math.pow(event.page.y - this.mouse.start.y, 2)));
		if (distance > this.options.snap){
			this.cancel();
			this.document.addEvents({
				mousemove: this.bound.drag,
				mouseup: this.bound.stop
			});
			this.fireEvent('start', [this.element, event]).fireEvent('snap', this.element);
		}
	},

	drag: function(event){
		if (this.options.preventDefault) event.preventDefault();
		this.mouse.now = event.page;
		for (var z in this.options.modifiers){
			if (!this.options.modifiers[z]) continue;
			this.value.now[z] = this.mouse.now[z] - this.mouse.pos[z];
			if (this.options.invert) this.value.now[z] *= -1;
			if (this._invert[z]) this.value.now[z] *= -1;
			if (this.options.limit && this.limit[z]){
				if ($chk(this.limit[z][1]) && (this.value.now[z] > this.limit[z][1])){
					this.value.now[z] = this.limit[z][1];
				} else if ($chk(this.limit[z][0]) && (this.value.now[z] < this.limit[z][0])){
					this.value.now[z] = this.limit[z][0];
				}
			}
			if (this.options.grid[z]) this.value.now[z] -= ((this.value.now[z] - (this.limit[z][0]||0)) % this.options.grid[z]);
			if (this.options.style) {
				this.element.setStyle(this.options.modifiers[z], this.value.now[z] + this.options.unit);
			} else {
				this.element[this.options.modifiers[z]] = this.value.now[z];
			}
		}
		this.fireEvent('drag', [this.element, event]);
	},

	cancel: function(event){
		this.document.removeEvent('mousemove', this.bound.check);
		this.document.removeEvent('mouseup', this.bound.cancel);
		if (event){
			this.document.removeEvent(this.selection, this.bound.eventStop);
			this.fireEvent('cancel', this.element);
		}
	},

	stop: function(event){
		this.document.removeEvent(this.selection, this.bound.eventStop);
		this.document.removeEvent('mousemove', this.bound.drag);
		this.document.removeEvent('mouseup', this.bound.stop);
		if (event) this.fireEvent('complete', [this.element, event]);
	}

});

Element.implement({

	makeResizable: function(options){
		var drag = new Drag(this, $merge({modifiers: {x: 'width', y: 'height'}}, options));
		this.store('resizer', drag);
		return drag.addEvent('drag', function(){
			this.fireEvent('resize', drag);
		}.bind(this));
	}

});


/*
---

script: Drag.Move.js

name: Drag.Move

description: A Drag extension that provides support for the constraining of draggables to containers and droppables.

license: MIT-style license

authors:
  - Valerio Proietti
  - Tom Occhinno
  - Jan Kassens
  - Aaron Newton
  - Scott Kyle

requires:
  - Core/Element.Dimensions
  - /Drag

provides: [Drag.Move]

...
*/

Drag.Move = new Class({

	Extends: Drag,

	options: {/*
		onEnter: $empty(thisElement, overed),
		onLeave: $empty(thisElement, overed),
		onDrop: $empty(thisElement, overed, event),*/
		droppables: [],
		container: false,
		precalculate: false,
		includeMargins: true,
		checkDroppables: true
	},

	initialize: function(element, options){
		this.parent(element, options);
		element = this.element;

		this.droppables = $$(this.options.droppables);
		this.container = document.id(this.options.container);

		if (this.container && $type(this.container) != 'element')
			this.container = document.id(this.container.getDocument().body);

		if (this.options.style){
			if (this.options.modifiers.x == "left" && this.options.modifiers.y == "top"){
				var parentStyles,
					parent = document.id(element.getOffsetParent());
				if (parent) parentStyles = parent.getStyles('border-top-width', 'border-left-width');

				var styles = element.getStyles('left', 'top');
				if (parent && (styles.left == 'auto' || styles.top == 'auto')){
					var parentPosition = element.getPosition(parent);
					parentPosition.x = parentPosition.x - (parentStyles['border-left-width'] ? parentStyles['border-left-width'].toInt() : 0);
					parentPosition.y = parentPosition.y - (parentStyles['border-top-width'] ? parentStyles['border-top-width'].toInt() : 0);
					element.setPosition(parentPosition);
				}
			}
			if (element.getStyle('position') == 'static') element.setStyle('position', 'absolute');
		}

		this.addEvent('start', this.checkDroppables, true);

		this.overed = null;
	},

	start: function(event){
		if (this.container) this.options.limit = this.calculateLimit();

		if (this.options.precalculate){
			this.positions = this.droppables.map(function(el){
				return el.getCoordinates();
			});
		}

		this.parent(event);
	},

	calculateLimit: function(){
		var offsetParent = document.id(this.element.getOffsetParent()) || document.body,
			containerCoordinates = this.container.getCoordinates(offsetParent),
			containerBorder = {},
			elementMargin = {},
			elementBorder = {},
			containerMargin = {},
			offsetParentBorder = {},
			offsetParentPadding = {};

		['top', 'right', 'bottom', 'left'].each(function(pad){
			containerBorder[pad] = this.container.getStyle('border-' + pad).toInt();
			elementBorder[pad] = this.element.getStyle('border-' + pad).toInt();
			elementMargin[pad] = this.element.getStyle('margin-' + pad).toInt();
			containerMargin[pad] = this.container.getStyle('margin-' + pad).toInt();
			offsetParentPadding[pad] = offsetParent.getStyle('padding-' + pad).toInt();
			offsetParentBorder[pad] = offsetParent.getStyle('border-' + pad).toInt();
		}, this);

		var width = this.element.offsetWidth + elementMargin.left + elementMargin.right,
			height = this.element.offsetHeight + elementMargin.top + elementMargin.bottom,
			left = 0,
			top = 0,
			right = containerCoordinates.right - containerBorder.right - width,
			bottom = containerCoordinates.bottom - containerBorder.bottom - height;

		if (this.options.includeMargins){
			left += elementMargin.left;
			top += elementMargin.top;
		} else {
			right += elementMargin.right;
			bottom += elementMargin.bottom;
		}

		if (this.element.getStyle('position') == 'relative'){
			var coords = this.element.getCoordinates(offsetParent);
			coords.left -= this.element.getStyle('left').toInt();
			coords.top -= this.element.getStyle('top').toInt();

			left += containerBorder.left - coords.left;
			top += containerBorder.top - coords.top;
			right += elementMargin.left - coords.left;
			bottom += elementMargin.top - coords.top;

			if (this.container != offsetParent){
				left += containerMargin.left + offsetParentPadding.left;
				top += (Browser.Engine.trident4 ? 0 : containerMargin.top) + offsetParentPadding.top;
			}
		} else {
			left -= elementMargin.left;
			top -= elementMargin.top;
			if (this.container == offsetParent){
				right -= containerBorder.left;
				bottom -= containerBorder.top;
			} else {
				left += containerCoordinates.left + containerBorder.left - offsetParentBorder.left;
				top += containerCoordinates.top + containerBorder.top - offsetParentBorder.top;
				right -= offsetParentBorder.left;
				bottom -= offsetParentBorder.top;
			}
		}

		return {
			x: [left, right],
			y: [top, bottom]
		};
	},

	checkAgainst: function(el, i){
		el = (this.positions) ? this.positions[i] : el.getCoordinates();
		var now = this.mouse.now;
		return (now.x > el.left && now.x < el.right && now.y < el.bottom && now.y > el.top);
	},

	checkDroppables: function(){
		var overed = this.droppables.filter(this.checkAgainst, this).getLast();
		if (this.overed != overed){
			if (this.overed) this.fireEvent('leave', [this.element, this.overed]);
			if (overed) this.fireEvent('enter', [this.element, overed]);
			this.overed = overed;
		}
	},

	drag: function(event){
		this.parent(event);
		if (this.options.checkDroppables && this.droppables.length) this.checkDroppables();
	},

	stop: function(event){
		this.checkDroppables();
		this.fireEvent('drop', [this.element, this.overed, event]);
		this.overed = null;
		return this.parent(event);
	}

});

Element.implement({

	makeDraggable: function(options){
		var drag = new Drag.Move(this, options);
		this.store('dragger', drag);
		return drag;
	}

});


/*
---

script: IframeShim.js

name: IframeShim

description: Defines IframeShim, a class for obscuring select lists and flash objects in IE.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Element.Event
  - Core/Element.Style
  - Core/Options
  - Core/Events
  - /Element.Position
  - /Class.Occlude

provides: [IframeShim]

...
*/

var IframeShim = new Class({

	Implements: [Options, Events, Class.Occlude],

	options: {
		className: 'iframeShim',
		src: 'javascript:false;document.write("");',
		display: false,
		zIndex: null,
		margin: 0,
		offset: {x: 0, y: 0},
		browsers: (Browser.Engine.trident4 || (Browser.Engine.gecko && !Browser.Engine.gecko19 && Browser.Platform.mac))
	},

	property: 'IframeShim',

	initialize: function(element, options){
		this.element = document.id(element);
		if (this.occlude()) return this.occluded;
		this.setOptions(options);
		this.makeShim();
		return this;
	},

	makeShim: function(){
		if(this.options.browsers){
			var zIndex = this.element.getStyle('zIndex').toInt();

			if (!zIndex){
				zIndex = 1;
				var pos = this.element.getStyle('position');
				if (pos == 'static' || !pos) this.element.setStyle('position', 'relative');
				this.element.setStyle('zIndex', zIndex);
			}
			zIndex = ($chk(this.options.zIndex) && zIndex > this.options.zIndex) ? this.options.zIndex : zIndex - 1;
			if (zIndex < 0) zIndex = 1;
			this.shim = new Element('iframe', {
				src: this.options.src,
				scrolling: 'no',
				frameborder: 0,
				styles: {
					zIndex: zIndex,
					position: 'absolute',
					border: 'none',
					filter: 'progid:DXImageTransform.Microsoft.Alpha(style=0,opacity=0)'
				},
				'class': this.options.className
			}).store('IframeShim', this);
			var inject = (function(){
				this.shim.inject(this.element, 'after');
				this[this.options.display ? 'show' : 'hide']();
				this.fireEvent('inject');
			}).bind(this);
			if (!IframeShim.ready) window.addEvent('load', inject);
			else inject();
		} else {
			this.position = this.hide = this.show = this.dispose = $lambda(this);
		}
	},

	position: function(){
		if (!IframeShim.ready || !this.shim) return this;
		var size = this.element.measure(function(){
			return this.getSize();
		});
		if (this.options.margin != undefined){
			size.x = size.x - (this.options.margin * 2);
			size.y = size.y - (this.options.margin * 2);
			this.options.offset.x += this.options.margin;
			this.options.offset.y += this.options.margin;
		}
		this.shim.set({width: size.x, height: size.y}).position({
			relativeTo: this.element,
			offset: this.options.offset
		});
		return this;
	},

	hide: function(){
		if (this.shim) this.shim.setStyle('display', 'none');
		return this;
	},

	show: function(){
		if (this.shim) this.shim.setStyle('display', 'block');
		return this.position();
	},

	dispose: function(){
		if (this.shim) this.shim.dispose();
		return this;
	},

	destroy: function(){
		if (this.shim) this.shim.destroy();
		return this;
	}

});

window.addEvent('load', function(){
	IframeShim.ready = true;
});


/*
---

script: HtmlTable.js

name: HtmlTable

description: Builds table elements with methods to add rows.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Options
  - Core/Events
  - /Class.Occlude

provides: [HtmlTable]

...
*/

var HtmlTable = new Class({

	Implements: [Options, Events, Class.Occlude],

	options: {
		properties: {
			cellpadding: 0,
			cellspacing: 0,
			border: 0
		},
		rows: [],
		headers: [],
		footers: []
	},

	property: 'HtmlTable',

	initialize: function(){
		var params = Array.link(arguments, {options: Object.type, table: Element.type});
		this.setOptions(params.options);
		this.element = params.table || new Element('table', this.options.properties);
		if (this.occlude()) return this.occluded;
		this.build();
	},

	build: function(){
		this.element.store('HtmlTable', this);

		this.body = document.id(this.element.tBodies[0]) || new Element('tbody').inject(this.element);
		$$(this.body.rows);

		if (this.options.headers.length) this.setHeaders(this.options.headers);
		else this.thead = document.id(this.element.tHead);
		if (this.thead) this.head = document.id(this.thead.rows[0]);

		if (this.options.footers.length) this.setFooters(this.options.footers);
		this.tfoot = document.id(this.element.tFoot);
		if (this.tfoot) this.foot = document.id(this.tfoot.rows[0]);

		this.options.rows.each(function(row){
			this.push(row);
		}, this);

		['adopt', 'inject', 'wraps', 'grab', 'replaces', 'dispose'].each(function(method){
				this[method] = this.element[method].bind(this.element);
		}, this);
	},

	toElement: function(){
		return this.element;
	},

	empty: function(){
		this.body.empty();
		return this;
	},

	set: function(what, items) {
		var target = (what == 'headers') ? 'tHead' : 'tFoot';
		this[target.toLowerCase()] = (document.id(this.element[target]) || new Element(target.toLowerCase()).inject(this.element, 'top')).empty();
		var data = this.push(items, {}, this[target.toLowerCase()], what == 'headers' ? 'th' : 'td');
		if (what == 'headers') this.head = document.id(this.thead.rows[0]);
		else this.foot = document.id(this.thead.rows[0]);
		return data;
	},

	setHeaders: function(headers){
		this.set('headers', headers);
		return this;
	},

	setFooters: function(footers){
		this.set('footers', footers);
		return this;
	},

	push: function(row, rowProperties, target, tag){
		if ($type(row) == "element" && row.get('tag') == 'tr') {
			row.inject(target || this.body);
			return {
				tr: row,
				tds: row.getChildren('td')
			};
		}
		var tds = row.map(function(data){
			var td = new Element(tag || 'td', data ? data.properties : {}),
				type = (data ? data.content : '') || data,
				element = document.id(type);
			if($type(type) != 'string' && element) td.adopt(element);
			else td.set('html', type);

			return td;
		});

		return {
			tr: new Element('tr', rowProperties).inject(target || this.body).adopt(tds),
			tds: tds
		};
	}

});


/*
---

script: HtmlTable.Zebra.js

name: HtmlTable.Zebra

description: Builds a stripy table with methods to add rows.

license: MIT-style license

authors:
  - Harald Kirschner
  - Aaron Newton

requires:
  - /HtmlTable
  - /Class.refactor

provides: [HtmlTable.Zebra]

...
*/

HtmlTable = Class.refactor(HtmlTable, {

	options: {
		classZebra: 'table-tr-odd',
		zebra: true
	},

	initialize: function(){
		this.previous.apply(this, arguments);
		if (this.occluded) return this.occluded;
		if (this.options.zebra) this.updateZebras();
	},

	updateZebras: function(){
		Array.each(this.body.rows, this.zebra, this);
	},

	zebra: function(row, i){
		return row[((i % 2) ? 'remove' : 'add')+'Class'](this.options.classZebra);
	},

	push: function(){
		var pushed = this.previous.apply(this, arguments);
		if (this.options.zebra) this.updateZebras();
		return pushed;
	}

});

/*
---

script: HtmlTable.Sort.js

name: HtmlTable.Sort

description: Builds a stripy, sortable table with methods to add rows.

license: MIT-style license

authors:
  - Harald Kirschner
  - Aaron Newton

requires:
  - Core/Hash
  - /HtmlTable
  - /Class.refactor
  - /Element.Delegation
  - /String.Extras
  - /Date

provides: [HtmlTable.Sort]

...
*/

HtmlTable = Class.refactor(HtmlTable, {

	options: {/*
		onSort: $empty, */
		sortIndex: 0,
		sortReverse: false,
		parsers: [],
		defaultParser: 'string',
		classSortable: 'table-sortable',
		classHeadSort: 'table-th-sort',
		classHeadSortRev: 'table-th-sort-rev',
		classNoSort: 'table-th-nosort',
		classGroupHead: 'table-tr-group-head',
		classGroup: 'table-tr-group',
		classCellSort: 'table-td-sort',
		classSortSpan: 'table-th-sort-span',
		sortable: false
	},

	initialize: function () {
		this.previous.apply(this, arguments);
		if (this.occluded) return this.occluded;
		this.sorted = {index: null, dir: 1};
		this.bound = {
			headClick: this.headClick.bind(this)
		};
		this.sortSpans = new Elements();
		if (this.options.sortable) {
			this.enableSort();
			if (this.options.sortIndex != null) this.sort(this.options.sortIndex, this.options.sortReverse);
		}
	},

	attachSorts: function(attach){
		this.element.removeEvents('click:relay(th)');
		this.element[$pick(attach, true) ? 'addEvent' : 'removeEvent']('click:relay(th)', this.bound.headClick);
	},

	setHeaders: function(){
		this.previous.apply(this, arguments);
		if (this.sortEnabled) this.detectParsers();
	},

	detectParsers: function(force){
		if (!this.head) return;
		var parsers = this.options.parsers,
				rows = this.body.rows;

		// auto-detect
		this.parsers = $$(this.head.cells).map(function(cell, index) {
			if (!force && (cell.hasClass(this.options.classNoSort) || cell.retrieve('htmltable-parser'))) return cell.retrieve('htmltable-parser');
			var thDiv = new Element('div');
			$each(cell.childNodes, function(node) {
				thDiv.adopt(node);
			});
			thDiv.inject(cell);
			var sortSpan = new Element('span', {'html': '&#160;', 'class': this.options.classSortSpan}).inject(thDiv, 'top');

			this.sortSpans.push(sortSpan);

			var parser = parsers[index],
					cancel;
			switch ($type(parser)) {
				case 'function': parser = {convert: parser}; cancel = true; break;
				case 'string': parser = parser; cancel = true; break;
			}
			if (!cancel) {
				HtmlTable.Parsers.some(function(current) {
					var match = current.match;
					if (!match) return false;
					for (var i = 0, j = rows.length; i < j; i++) {
						var cell = document.id(rows[i].cells[index]);
						var text = cell ? cell.get('html').clean() : '';
						if (text && match.test(text)) {
							parser = current;
							return true;
						}
					}
				});
			}

			if (!parser) parser = this.options.defaultParser;
			cell.store('htmltable-parser', parser);
			return parser;
		}, this);
	},

	headClick: function(event, el) {
		if (!this.head || el.hasClass(this.options.classNoSort)) return;
		var index = Array.indexOf(this.head.cells, el);
		this.sort(index);
		return false;
	},

	sort: function(index, reverse, pre) {
		if (!this.head) return;
		pre = !!(pre);
		var classCellSort = this.options.classCellSort;
		var classGroup = this.options.classGroup,
				classGroupHead = this.options.classGroupHead;

		if (!pre) {
			if (index != null) {
				if (this.sorted.index == index) {
					this.sorted.reverse = !(this.sorted.reverse);
				} else {
					if (this.sorted.index != null) {
						this.sorted.reverse = false;
						this.head.cells[this.sorted.index].removeClass(this.options.classHeadSort).removeClass(this.options.classHeadSortRev);
					} else {
						this.sorted.reverse = true;
					}
					this.sorted.index = index;
				}
			} else {
				index = this.sorted.index;
			}

			if (reverse != null) this.sorted.reverse = reverse;

			var head = document.id(this.head.cells[index]);
			if (head) {
				head.addClass(this.options.classHeadSort);
				if (this.sorted.reverse) head.addClass(this.options.classHeadSortRev);
				else head.removeClass(this.options.classHeadSortRev);
			}

			this.body.getElements('td').removeClass(this.options.classCellSort);
		}

		var parser = this.parsers[index];
		if ($type(parser) == 'string') parser = HtmlTable.Parsers.get(parser);
		if (!parser) return;

		if (!Browser.Engine.trident) {
			var rel = this.body.getParent();
			this.body.dispose();
		}

		var data = Array.map(this.body.rows, function(row, i) {
			var value = parser.convert.call(document.id(row.cells[index]));

			return {
				position: i,
				value: value,
				toString:  function() {
					return value.toString();
				}
			};
		}, this);
		data.reverse(true);

		data.sort(function(a, b){
			if (a.value === b.value) return 0;
			return a.value > b.value ? 1 : -1;
		});

		if (!this.sorted.reverse) data.reverse(true);

		var i = data.length, body = this.body;
		var j, position, entry, group;

		while (i) {
			var item = data[--i];
			position = item.position;
			var row = body.rows[position];
			if (row.disabled) continue;

			if (!pre) {
				if (group === item.value) {
					row.removeClass(classGroupHead).addClass(classGroup);
				} else {
					group = item.value;
					row.removeClass(classGroup).addClass(classGroupHead);
				}
				if (this.options.zebra) this.zebra(row, i);

				row.cells[index].addClass(classCellSort);
			}

			body.appendChild(row);
			for (j = 0; j < i; j++) {
				if (data[j].position > position) data[j].position--;
			}
		};
		data = null;
		if (rel) rel.grab(body);

		return this.fireEvent('sort', [body, index]);
	},

	reSort: function(){
		if (this.sortEnabled) this.sort.call(this, this.sorted.index, this.sorted.reverse);
		return this;
	},

	enableSort: function(){
		this.element.addClass(this.options.classSortable);
		this.attachSorts(true);
		this.detectParsers();
		this.sortEnabled = true;
		return this;
	},

	disableSort: function(){
		this.element.removeClass(this.options.classSortable);
		this.attachSorts(false);
		this.sortSpans.each(function(span) { span.destroy(); });
		this.sortSpans.empty();
		this.sortEnabled = false;
		return this;
	}

});

HtmlTable.Parsers = new Hash({

	'date': {
		match: /^\d{2}[-\/ ]\d{2}[-\/ ]\d{2,4}$/,
		convert: function() {
			var d = Date.parse(this.get('text').stripTags());
			return $type(d) == 'date' ? d.format('db') : '';
		},
		type: 'date'
	},
	'input-checked': {
		match: / type="(radio|checkbox)" /,
		convert: function() {
			return this.getElement('input').checked;
		}
	},
	'input-value': {
		match: /<input/,
		convert: function() {
			return this.getElement('input').value;
		}
	},
	'number': {
		match: /^\d+[^\d.,]*$/,
		convert: function() {
			return this.get('text').stripTags().toInt();
		},
		number: true
	},
	'numberLax': {
		match: /^[^\d]+\d+$/,
		convert: function() {
			return this.get('text').replace(/[^-?^0-9]/, '').stripTags().toInt();
		},
		number: true
	},
	'float': {
		match: /^[\d]+\.[\d]+/,
		convert: function() {
			return this.get('text').replace(/[^-?^\d.]/, '').stripTags().toFloat();
		},
		number: true
	},
	'floatLax': {
		match: /^[^\d]+[\d]+\.[\d]+$/,
		convert: function() {
			return this.get('text').replace(/[^-?^\d.]/, '').stripTags();
		},
		number: true
	},
	'string': {
		match: null,
		convert: function() {
			return this.get('text').stripTags();
		}
	},
	'title': {
		match: null,
		convert: function() {
			return this.title;
		}
	}

});


HtmlTable.defineParsers = function(parsers){
	HtmlTable.Parsers = new Hash(parsers).combine(HtmlTable.Parsers);
};


/*
---

script: HtmlTable.Select.js

name: HtmlTable.Select

description: Builds a stripy, sortable table with methods to add rows. Rows can be selected with the mouse or keyboard navigation.

license: MIT-style license

authors:
  - Harald Kirschner
  - Aaron Newton

requires:
  - /Keyboard.Extras
  - /HtmlTable
  - /Class.refactor
  - /Element.Delegation
  - /Element.Shortcuts

provides: [HtmlTable.Select]

...
*/

HtmlTable = Class.refactor(HtmlTable, {

	options: {
		/*onRowFocus: $empty,
		onRowUnfocus: $empty,*/
		useKeyboard: true,
		classRowSelected: 'table-tr-selected',
		classRowHovered: 'table-tr-hovered',
		classSelectable: 'table-selectable',
		shiftForMultiSelect: true,
		allowMultiSelect: true,
		selectable: false
	},

	initialize: function(){
		this.previous.apply(this, arguments);
		if (this.occluded) return this.occluded;
		this._selectedRows = new Elements();
		this._bound = {
			mouseleave: this._mouseleave.bind(this),
			clickRow: this._clickRow.bind(this)
		};
		if (this.options.selectable) this.enableSelect();
	},

	enableSelect: function(){
		this._selectEnabled = true;
		this._attachSelects();
		this.element.addClass(this.options.classSelectable);
	},

	disableSelect: function(){
		this._selectEnabled = false;
		this._attachSelects(false);
		this.element.removeClass(this.options.classSelectable);
	},

	push: function(){
		var ret = this.previous.apply(this, arguments);
		this._updateSelects();
		return ret;
	},

	toggleRow: function(row){
		return this.isSelected(row) ? this.deselectRow.apply(this, arguments) : this.selectRow.apply(this, arguments);
	},

	selectRow: function(row, _nocheck){
		//private variable _nocheck: boolean whether or not to confirm the row is in the table body
		//added here for optimization when selecting ranges
		if (this.isSelected(row) || (!_nocheck && !this.body.getChildren().contains(row))) return;
		if (!this.options.allowMultiSelect) this.selectNone();

		if (!this.isSelected(row)) {
			this._selectedRows.push(row);
			row.addClass(this.options.classRowSelected);
			this.fireEvent('rowFocus', [row, this._selectedRows]);
		}
		this._focused = row;
		document.clearSelection();
		return this;
	},

	isSelected: function(row){
		return this._selectedRows.contains(row);
	},

	deselectRow: function(row, _nocheck){
		if (!this.isSelected(row) || (!_nocheck && !this.body.getChildren().contains(row))) return;
		this._selectedRows.erase(row);
		row.removeClass(this.options.classRowSelected);
		this.fireEvent('rowUnfocus', [row, this._selectedRows]);
		return this;
	},

	selectAll: function(selectNone){
		if (!selectNone && !this.options.allowMultiSelect) return;
		this.selectRange(0, this.body.rows.length, selectNone);
		return this;
	},

	selectNone: function(){
		return this.selectAll(true);
	},

	selectRange: function(startRow, endRow, _deselect){
		if (!this.options.allowMultiSelect && !_deselect) return;
		var method = _deselect ? 'deselectRow' : 'selectRow',
		    rows = $A(this.body.rows);

		if ($type(startRow) == 'element') startRow = rows.indexOf(startRow);
		if ($type(endRow) == 'element') endRow = rows.indexOf(endRow);
		endRow = endRow < rows.length - 1 ? endRow : rows.length - 1;

		if (endRow < startRow) {
			var tmp = startRow;
			startRow = endRow;
			endRow = tmp;
		}

		for(var i = startRow; i <= endRow; i++) this[method](rows[i], true);

		return this;
	},

	deselectRange: function(startRow, endRow){
		this.selectRange(startRow, endRow, true);
	},
/*
	Private methods:
*/

	_enterRow: function(row){
		if (this._hovered) this._hovered = this._leaveRow(this._hovered);
		this._hovered = row.addClass(this.options.classRowHovered);
	},

	_leaveRow: function(row){
		row.removeClass(this.options.classRowHovered);
	},

	_updateSelects: function(){
		Array.each(this.body.rows, function(row){
			var binders = row.retrieve('binders');
			if ((binders && this._selectEnabled) || (!binders && !this._selectEnabled)) return;
			if (!binders){
				binders = {
					mouseenter: this._enterRow.bind(this, [row]),
					mouseleave: this._leaveRow.bind(this, [row])
				};
				row.store('binders', binders).addEvents(binders);
			} else {
				row.removeEvents(binders);
			}
		}, this);
	},

	_shiftFocus: function(offset, event){
		if (!this._focused) return this.selectRow(this.body.rows[0], event);
		var to = this._getRowByOffset(offset);
		if (to === null || this._focused == this.body.rows[to]) return this;
		this.toggleRow(this.body.rows[to], event);
	},

	_clickRow: function(event, row){
		var selecting = (event.shift || event.meta || event.control) && this.options.shiftForMultiSelect;
		if (!selecting && !(event.rightClick && this.isSelected(row) && this.options.allowMultiSelect)) this.selectNone();
		if (event.rightClick) this.selectRow(row);
		else this.toggleRow(row);
		if (event.shift) {
			this.selectRange(this._rangeStart || this.body.rows[0], row, this._rangeStart ? !this.isSelected(row) : true);
			this._focused = row;
		}
		this._rangeStart = row;
	},

	_getRowByOffset: function(offset){
		if (!this._focused) return 0;
		var index = Array.indexOf(this.body.rows, this._focused) + offset;
		if (index < 0) index = null;
		if (index >= this.body.rows.length) index = null;
		return index;
	},

	_attachSelects: function(attach){
		attach = $pick(attach, true);
		var method = attach ? 'addEvents' : 'removeEvents';
		this.element[method]({
			mouseleave: this._bound.mouseleave
		});
		this.body[method]({
			'click:relay(tr)': this._bound.clickRow,
			'contextmenu:relay(tr)': this._bound.clickRow
		});
		if (this.options.useKeyboard || this.keyboard){
			if (!this.keyboard) {
				var timer, held;
				var move = function(offset){
					var mover = function(e){
						$clear(timer);
						e.preventDefault();
						var to = this.body.rows[this._getRowByOffset(offset)];
						if (e.shift && to && this.isSelected(to)) {
							this.deselectRow(this._focused);
							this._focused = to;
						} else {
							if (to && (!this.options.allowMultiSelect || !e.shift)) {
								this.selectNone();
							}
							this._shiftFocus(offset, e);
						}
						if (held) {
							timer = mover.delay(100, this, e);
						} else {
							timer = (function(){
								held = true;
								mover(e);
							}).delay(400);
						}
					}.bind(this);
					return mover;
				}.bind(this);

				var clear = function(){
					$clear(timer);
					held = false;
				};

				this.keyboard = new Keyboard({
					events: {
						'keydown:shift+up': move(-1),
						'keydown:shift+down': move(1),
						'keyup:shift+up': clear,
						'keyup:shift+down': clear,
						'keyup:up': clear,
						'keyup:down': clear
					},
					active: true
				});

				var shiftHint = '';
				if (this.options.allowMultiSelect && this.options.shiftForMultiSelect && this.options.useKeyboard) {
					shiftHint = " (Shift multi-selects).";
				}

				this.keyboard.addShortcuts({
					'Select Previous Row': {
						keys: 'up',
						shortcut: 'up arrow',
						handler: move(-1),
						description: 'Select the previous row in the table.' + shiftHint
					},
					'Select Next Row': {
						keys: 'down',
						shortcut: 'down arrow',
						handler: move(1),
						description: 'Select the next row in the table.' + shiftHint
					}
				});
			}
			this.keyboard[attach ? 'activate' : 'deactivate']();
		}
		this._updateSelects();
	},

	_mouseleave: function(){
		if (this._hovered) this._leaveRow(this._hovered);
	}

});

/*
---

script: Keyboard.js

name: Keyboard

description: KeyboardEvents used to intercept events on a class for keyboard and format modifiers in a specific order so as to make alt+shift+c the same as shift+alt+c.

license: MIT-style license

authors:
  - Perrin Westrich
  - Aaron Newton
  - Scott Kyle

requires:
  - Core/Events
  - Core/Options
  - Core/Element.Event
  - /Log

provides: [Keyboard]

...
*/

(function(){

	var Keyboard = this.Keyboard = new Class({

		Extends: Events,

		Implements: [Options, Log],

		options: {
			/*
			onActivate: $empty,
			onDeactivate: $empty,
			*/
			defaultEventType: 'keydown',
			active: false,
			manager: null,
			events: {},
			nonParsedEvents: ['activate', 'deactivate', 'onactivate', 'ondeactivate', 'changed', 'onchanged']
		},

		initialize: function(options){
			if (options && options.manager) {
				this.manager = options.manager;
				delete options.manager;
			}
			this.setOptions(options);
			this.setup();
		},
		setup: function(){
			this.addEvents(this.options.events);
			//if this is the root manager, nothing manages it
			if (Keyboard.manager && !this.manager) Keyboard.manager.manage(this);
			if (this.options.active) this.activate();
		},

		handle: function(event, type){
			//Keyboard.stop(event) prevents key propagation
			if (event.preventKeyboardPropagation) return;

			var bubbles = !!this.manager;
			if (bubbles && this.activeKB){
				this.activeKB.handle(event, type);
				if (event.preventKeyboardPropagation) return;
			}
			this.fireEvent(type, event);

			if (!bubbles && this.activeKB) this.activeKB.handle(event, type);
		},

		addEvent: function(type, fn, internal){
			return this.parent(Keyboard.parse(type, this.options.defaultEventType, this.options.nonParsedEvents), fn, internal);
		},

		removeEvent: function(type, fn){
			return this.parent(Keyboard.parse(type, this.options.defaultEventType, this.options.nonParsedEvents), fn);
		},

		toggleActive: function(){
			return this[this.active ? 'deactivate' : 'activate']();
		},

		activate: function(instance){
			if (instance) {
				if (instance.isActive()) return this;
				//if we're stealing focus, store the last keyboard to have it so the relinquish command works
				if (this.activeKB && instance != this.activeKB) {
					this.previous = this.activeKB;
					this.previous.fireEvent('deactivate');
				}
				//if we're enabling a child, assign it so that events are now passed to it
				this.activeKB = instance.fireEvent('activate');
				Keyboard.manager.fireEvent('changed');
			} else if (this.manager) {
				//else we're enabling ourselves, we must ask our parent to do it for us
				this.manager.activate(this);
			}
			return this;
		},

		isActive: function(){
			return this.manager ? this.manager.activeKB == this :  Keyboard.manager == this;
		},

		deactivate: function(instance){
			if (instance) {
				if(instance === this.activeKB) {
					this.activeKB = null;
					instance.fireEvent('deactivate');
					Keyboard.manager.fireEvent('changed');
				}
			} else if (this.manager) {
				this.manager.deactivate(this);
			}
			return this;
		},

		relinquish: function(){
			if (this.isActive() && this.manager && this.manager.previous) this.manager.activate(this.manager.previous);
		},

		//management logic
		manage: function(instance){
			if (instance.manager && instance.manager != Keyboard.manager && this != Keyboard.manager) instance.manager.drop(instance);
			this.instances.push(instance);
			instance.manager = this;
			if (!this.activeKB) this.activate(instance);
		},

		_disable: function(instance){
			if (this.activeKB == instance) this.activeKB = null;
		},

		drop: function(instance){
			this._disable(instance);
			this.instances.erase(instance);
			Keyboard.manager.manage(instance);
			if (this.activeKB == instance && this.previous && this.instances.contains(this.previous)) this.activate(this.previous);
		},

		instances: [],

		trace: function(){
			Keyboard.trace(this);
		},

		each: function(fn){
			Keyboard.each(this, fn);
		}

	});

	var parsed = {};
	var modifiers = ['shift', 'control', 'alt', 'meta'];
	var regex = /^(?:shift|control|ctrl|alt|meta)$/;

	Keyboard.parse = function(type, eventType, ignore){
		if (ignore && ignore.contains(type.toLowerCase())) return type;

		type = type.toLowerCase().replace(/^(keyup|keydown):/, function($0, $1){
			eventType = $1;
			return '';
		});

		if (!parsed[type]){
			var key, mods = {};
			type.split('+').each(function(part){
				if (regex.test(part)) mods[part] = true;
				else key = part;
			});

			mods.control = mods.control || mods.ctrl; // allow both control and ctrl

			var keys = [];
			modifiers.each(function(mod){
				if (mods[mod]) keys.push(mod);
			});

			if (key) keys.push(key);
			parsed[type] = keys.join('+');
		}

		return eventType + ':' + parsed[type];
	};

	Keyboard.each = function(keyboard, fn){
		var current = keyboard || Keyboard.manager;
		while (current){
			fn.run(current);
			current = current.activeKB;
		}
	};

	Keyboard.stop = function(event){
		event.preventKeyboardPropagation = true;
	};

	Keyboard.manager = new Keyboard({
		active: true
	});

	Keyboard.trace = function(keyboard){
		keyboard = keyboard || Keyboard.manager;
		keyboard.enableLog();
		keyboard.log('the following items have focus: ');
		Keyboard.each(keyboard, function(current){
			keyboard.log(document.id(current.widget) || current.wiget || current);
		});
	};

	var handler = function(event){
		var keys = [];
		modifiers.each(function(mod){
			if (event[mod]) keys.push(mod);
		});

		if (!regex.test(event.key)) keys.push(event.key);
		Keyboard.manager.handle(event, event.type + ':' + keys.join('+'));
	};

	document.addEvents({
		'keyup': handler,
		'keydown': handler
	});

	Event.Keys.extend({
		'shift': 16,
		'control': 17,
		'alt': 18,
		'capslock': 20,
		'pageup': 33,
		'pagedown': 34,
		'end': 35,
		'home': 36,
		'numlock': 144,
		'scrolllock': 145,
		';': 186,
		'=': 187,
		',': 188,
		'-': Browser.Engine.gecko ? 109 : 189,
		'.': 190,
		'/': 191,
		'`': 192,
		'[': 219,
		'\\': 220,
		']': 221,
		"'": 222
	});

})();


/*
---

script: Mask.js

name: Mask

description: Creates a mask element to cover another.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Options
  - Core/Events
  - Core/Element.Event
  - /Class.Binds
  - /Element.Position
  - /IframeShim

provides: [Mask]

...
*/

var Mask = new Class({

	Implements: [Options, Events],

	Binds: ['position'],

	options: {
		// onShow: $empty,
		// onHide: $empty,
		// onDestroy: $empty,
		// onClick: $empty,
		//inject: {
		//  where: 'after',
		//  target: null,
		//},
		// hideOnClick: false,
		// id: null,
		// destroyOnHide: false,
		style: {},
		'class': 'mask',
		maskMargins: false,
		useIframeShim: true,
		iframeShimOptions: {}
	},

	initialize: function(target, options){
		this.target = document.id(target) || document.id(document.body);
		this.target.store('Mask', this);
		this.setOptions(options);
		this.render();
		this.inject();
	},

	render: function() {
		this.element = new Element('div', {
			'class': this.options['class'],
			id: this.options.id || 'mask-' + $time(),
			styles: $merge(this.options.style, {
				display: 'none'
			}),
			events: {
				click: function(){
					this.fireEvent('click');
					if (this.options.hideOnClick) this.hide();
				}.bind(this)
			}
		});
		this.hidden = true;
	},

	toElement: function(){
		return this.element;
	},

	inject: function(target, where){
		where = where || this.options.inject ? this.options.inject.where : '' || this.target == document.body ? 'inside' : 'after';
		target = target || this.options.inject ? this.options.inject.target : '' || this.target;
		this.element.inject(target, where);
		if (this.options.useIframeShim) {
			this.shim = new IframeShim(this.element, this.options.iframeShimOptions);
			this.addEvents({
				show: this.shim.show.bind(this.shim),
				hide: this.shim.hide.bind(this.shim),
				destroy: this.shim.destroy.bind(this.shim)
			});
		}
	},

	position: function(){
		this.resize(this.options.width, this.options.height);
		this.element.position({
			relativeTo: this.target,
			position: 'topLeft',
			ignoreMargins: !this.options.maskMargins,
			ignoreScroll: this.target == document.body
		});
		return this;
	},

	resize: function(x, y){
		var opt = {
			styles: ['padding', 'border']
		};
		if (this.options.maskMargins) opt.styles.push('margin');
		var dim = this.target.getComputedSize(opt);
		if (this.target == document.body) {
			var win = window.getScrollSize();
			if (dim.totalHeight < win.y) dim.totalHeight = win.y;
			if (dim.totalWidth < win.x) dim.totalWidth = win.x;
		}
		this.element.setStyles({
			width: $pick(x, dim.totalWidth, dim.x),
			height: $pick(y, dim.totalHeight, dim.y)
		});
		return this;
	},

	show: function(){
		if (!this.hidden) return this;
		window.addEvent('resize', this.position);
		this.position();
		this.showMask.apply(this, arguments);
		return this;
	},

	showMask: function(){
		this.element.setStyle('display', 'block');
		this.hidden = false;
		this.fireEvent('show');
	},

	hide: function(){
		if (this.hidden) return this;
		window.removeEvent('resize', this.position);
		this.hideMask.apply(this, arguments);
		if (this.options.destroyOnHide) return this.destroy();
		return this;
	},

	hideMask: function(){
		this.element.setStyle('display', 'none');
		this.hidden = true;
		this.fireEvent('hide');
	},

	toggle: function(){
		this[this.hidden ? 'show' : 'hide']();
	},

	destroy: function(){
		this.hide();
		this.element.destroy();
		this.fireEvent('destroy');
		this.target.eliminate('mask');
	}

});

Element.Properties.mask = {

	set: function(options){
		var mask = this.retrieve('mask');
		return this.eliminate('mask').store('mask:options', options);
	},

	get: function(options){
		if (options || !this.retrieve('mask')){
			if (this.retrieve('mask')) this.retrieve('mask').destroy();
			if (options || !this.retrieve('mask:options')) this.set('mask', options);
			this.store('mask', new Mask(this, this.retrieve('mask:options')));
		}
		return this.retrieve('mask');
	}

};

Element.implement({

	mask: function(options){
		this.get('mask', options).show();
		return this;
	},

	unmask: function(){
		this.get('mask').hide();
		return this;
	}

});

/*
---

script: Tips.js

name: Tips

description: Class for creating nice tips that follow the mouse cursor when hovering an element.

license: MIT-style license

authors:
  - Valerio Proietti
  - Christoph Pojer

requires:
  - Core/Options
  - Core/Events
  - Core/Element.Event
  - Core/Element.Style
  - Core/Element.Dimensions
  - /MooTools.More

provides: [Tips]

...
*/

(function(){

var read = function(option, element){
	return (option) ? ($type(option) == 'function' ? option(element) : element.get(option)) : '';
};

this.Tips = new Class({

	Implements: [Events, Options],

	options: {
		/*
		onAttach: $empty(element),
		onDetach: $empty(element),
		*/
		onShow: function(){
			this.tip.setStyle('display', 'block');
		},
		onHide: function(){
			this.tip.setStyle('display', 'none');
		},
		title: 'title',
		text: function(element){
			return element.get('rel') || element.get('href');
		},
		showDelay: 100,
		hideDelay: 100,
		className: 'tip-wrap',
		offset: {x: 16, y: 16},
		windowPadding: {x:0, y:0},
		fixed: false
	},

	initialize: function(){
		var params = Array.link(arguments, {options: Object.type, elements: $defined});
		this.setOptions(params.options);
		if (params.elements) this.attach(params.elements);
		this.container = new Element('div', {'class': 'tip'});
	},

	toElement: function(){
		if (this.tip) return this.tip;

		return this.tip = new Element('div', {
			'class': this.options.className,
			styles: {
				position: 'absolute',
				top: 0,
				left: 0
			}
		}).adopt(
			new Element('div', {'class': 'tip-top'}),
			this.container,
			new Element('div', {'class': 'tip-bottom'})
		);
	},

	attach: function(elements){
		$$(elements).each(function(element){
			var title = read(this.options.title, element),
				text = read(this.options.text, element);

			element.erase('title').store('tip:native', title).retrieve('tip:title', title);
			element.retrieve('tip:text', text);
			this.fireEvent('attach', [element]);

			var events = ['enter', 'leave'];
			if (!this.options.fixed) events.push('move');

			events.each(function(value){
				var event = element.retrieve('tip:' + value);
				if (!event) event = this['element' + value.capitalize()].bindWithEvent(this, element);

				element.store('tip:' + value, event).addEvent('mouse' + value, event);
			}, this);
		}, this);

		return this;
	},

	detach: function(elements){
		$$(elements).each(function(element){
			['enter', 'leave', 'move'].each(function(value){
				element.removeEvent('mouse' + value, element.retrieve('tip:' + value)).eliminate('tip:' + value);
			});

			this.fireEvent('detach', [element]);

			if (this.options.title == 'title'){ // This is necessary to check if we can revert the title
				var original = element.retrieve('tip:native');
				if (original) element.set('title', original);
			}
		}, this);

		return this;
	},

	elementEnter: function(event, element){
		this.container.empty();

		['title', 'text'].each(function(value){
			var content = element.retrieve('tip:' + value);
			if (content) this.fill(new Element('div', {'class': 'tip-' + value}).inject(this.container), content);
		}, this);

		$clear(this.timer);
		this.timer = (function(){
			this.show(element);
			this.position((this.options.fixed) ? {page: element.getPosition()} : event);
		}).delay(this.options.showDelay, this);
	},

	elementLeave: function(event, element){
		$clear(this.timer);
		this.timer = this.hide.delay(this.options.hideDelay, this, element);
		this.fireForParent(event, element);
	},

	fireForParent: function(event, element){
		element = element.getParent();
		if (!element || element == document.body) return;
		if (element.retrieve('tip:enter')) element.fireEvent('mouseenter', event);
		else this.fireForParent(event, element);
	},

	elementMove: function(event, element){
		this.position(event);
	},

	position: function(event){
		if (!this.tip) document.id(this);

		var size = window.getSize(), scroll = window.getScroll(),
			tip = {x: this.tip.offsetWidth, y: this.tip.offsetHeight},
			props = {x: 'left', y: 'top'},
			obj = {};

		for (var z in props){
			obj[props[z]] = event.page[z] + this.options.offset[z];
			if ((obj[props[z]] + tip[z] - scroll[z]) > size[z] - this.options.windowPadding[z]) obj[props[z]] = event.page[z] - this.options.offset[z] - tip[z];
		}

		this.tip.setStyles(obj);
	},

	fill: function(element, contents){
		if(typeof contents == 'string') element.set('html', contents);
		else element.adopt(contents);
	},

	show: function(element){
		if (!this.tip) document.id(this);
		if (!this.tip.getParent()) this.tip.inject(document.body);
		this.fireEvent('show', [this.tip, element]);
	},

	hide: function(element){
		if (!this.tip) document.id(this);
		this.fireEvent('hide', [this.tip, element]);
	}

});

})();


/*
---

script: Spinner.js

name: Spinner

description: Adds a semi-transparent overlay over a dom element with a spinnin ajax icon.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - Core/Fx.Tween
  - Core/Request
  - /Class.refactor
  - /Mask

provides: [Spinner]

...
*/

var Spinner = new Class({

	Extends: Mask,

	options: {
		/*message: false,*/
		'class':'spinner',
		containerPosition: {},
		content: {
			'class':'spinner-content'
		},
		messageContainer: {
			'class':'spinner-msg'
		},
		img: {
			'class':'spinner-img'
		},
		fxOptions: {
			link: 'chain'
		}
	},

	initialize: function(){
		this.parent.apply(this, arguments);
		this.target.store('spinner', this);

		//add this to events for when noFx is true; parent methods handle hide/show
		var deactivate = function(){ this.active = false; }.bind(this);
		this.addEvents({
			hide: deactivate,
			show: deactivate
		});
	},

	render: function(){
		this.parent();
		this.element.set('id', this.options.id || 'spinner-'+$time());
		this.content = document.id(this.options.content) || new Element('div', this.options.content);
		this.content.inject(this.element);
		if (this.options.message) {
			this.msg = document.id(this.options.message) || new Element('p', this.options.messageContainer).appendText(this.options.message);
			this.msg.inject(this.content);
		}
		if (this.options.img) {
			this.img = document.id(this.options.img) || new Element('div', this.options.img);
			this.img.inject(this.content);
		}
		this.element.set('tween', this.options.fxOptions);
	},

	show: function(noFx){
		if (this.active) return this.chain(this.show.bind(this));
		if (!this.hidden) {
			this.callChain.delay(20, this);
			return this;
		}
		this.active = true;
		return this.parent(noFx);
	},

	showMask: function(noFx){
		var pos = function(){
			this.content.position($merge({
				relativeTo: this.element
			}, this.options.containerPosition));
		}.bind(this);
		if (noFx) {
			this.parent();
			pos();
		} else {
			this.element.setStyles({
				display: 'block',
				opacity: 0
			}).tween('opacity', this.options.style.opacity || 0.9);
			pos();
			this.hidden = false;
			this.fireEvent('show');
			this.callChain();
		}
	},

	hide: function(noFx){
		if (this.active) return this.chain(this.hide.bind(this));
		if (this.hidden) {
			this.callChain.delay(20, this);
			return this;
		}
		this.active = true;
		return this.parent(noFx);
	},

	hideMask: function(noFx){
		if (noFx) return this.parent();
		this.element.tween('opacity', 0).get('tween').chain(function(){
			this.element.setStyle('display', 'none');
			this.hidden = true;
			this.fireEvent('hide');
			this.callChain();
		}.bind(this));
	},

	destroy: function(){
		this.content.destroy();
		this.parent();
		this.target.eliminate('spinner');
	}

});

Spinner.implement(new Chain);

Request = Class.refactor(Request, {

	options: {
		useSpinner: false,
		spinnerOptions: {},
		spinnerTarget: false
	},

	initialize: function(options){
		this._send = this.send;
		this.send = function(options){
			var spinner = this.getSpinner();
			if (spinner) spinner.chain(this._send.bind(this, options)).show();
			else this._send(options);
			return this;
		};
		this.previous(options);
	},

	getSpinner: function(){
		if (!this.spinner) {
			var update = document.id(this.options.spinnerTarget) || document.id(this.options.update);
			if (this.options.useSpinner && update) {
				this.spinner = update.get('spinner', this.options.spinnerOptions);
				['onComplete', 'onException', 'onCancel'].each(function(event){
					this.addEvent(event, this.spinner.hide.bind(this.spinner));
				}, this);
			}
		}
		return this.spinner;
	}

});

Element.Properties.spinner = {

	set: function(options){
		var spinner = this.retrieve('spinner');
		return this.eliminate('spinner').store('spinner:options', options);
	},

	get: function(options){
		if (options || !this.retrieve('spinner')){
			if (this.retrieve('spinner')) this.retrieve('spinner').destroy();
			if (options || !this.retrieve('spinner:options')) this.set('spinner', options);
			new Spinner(this, this.retrieve('spinner:options'));
		}
		return this.retrieve('spinner');
	}

};

Element.implement({

	spin: function(options){
		this.get('spinner', options).show();
		return this;
	},

	unspin: function(){
		var opt = Array.link(arguments, {options: Object.type, callback: Function.type});
		this.get('spinner', opt.options).hide(opt.callback);
		return this;
	}

});

/*
---

script: Date.English.US.js

name: Date.English.US

description: Date messages for US English.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - /Lang

provides: [Date.English.US]

...
*/

MooTools.lang.set('en-US', 'Date', {

	months: ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'],
	days: ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'],

	// Culture's date order: MM/DD/YYYY
	dateOrder: ['month', 'date', 'year'],
	shortDate: '%m/%d/%Y',
	shortTime: '%I:%M%p',
	AM: 'AM',
	PM: 'PM',

	// Date.Extras
	ordinal: function(dayOfMonth){
		// 1st, 2nd, 3rd, etc.
		return (dayOfMonth > 3 && dayOfMonth < 21) ? 'th' : ['th', 'st', 'nd', 'rd', 'th'][Math.min(dayOfMonth % 10, 4)];
	},

	lessThanMinuteAgo: 'less than a minute ago',
	minuteAgo: 'about a minute ago',
	minutesAgo: '{delta} minutes ago',
	hourAgo: 'about an hour ago',
	hoursAgo: 'about {delta} hours ago',
	dayAgo: '1 day ago',
	daysAgo: '{delta} days ago',
	weekAgo: '1 week ago',
	weeksAgo: '{delta} weeks ago',
	monthAgo: '1 month ago',
	monthsAgo: '{delta} months ago',
	yearAgo: '1 year ago',
	yearsAgo: '{delta} years ago',

	lessThanMinuteUntil: 'less than a minute from now',
	minuteUntil: 'about a minute from now',
	minutesUntil: '{delta} minutes from now',
	hourUntil: 'about an hour from now',
	hoursUntil: 'about {delta} hours from now',
	dayUntil: '1 day from now',
	daysUntil: '{delta} days from now',
	weekUntil: '1 week from now',
	weeksUntil: '{delta} weeks from now',
	monthUntil: '1 month from now',
	monthsUntil: '{delta} months from now',
	yearUntil: '1 year from now',
	yearsUntil: '{delta} years from now'

});


/*
---

script: Form.Validator.English.js

name: Form.Validator.English

description: Form Validator messages for English.

license: MIT-style license

authors:
  - Aaron Newton

requires:
  - /Lang

provides: [Form.Validator.English]

...
*/

MooTools.lang.set('en-US', 'Form.Validator', {

	required: 'This field is required.',
	minLength: 'Please enter at least {minLength} characters (you entered {length} characters).',
	maxLength: 'Please enter no more than {maxLength} characters (you entered {length} characters).',
	integer: 'Please enter an integer in this field. Numbers with decimals (e.g. 1.25) are not permitted.',
	numeric: 'Please enter only numeric values in this field (i.e. "1" or "1.1" or "-1" or "-1.1").',
	digits: 'Please use numbers and punctuation only in this field (for example, a phone number with dashes or dots is permitted).',
	alpha: 'Please use only letters (a-z) within this field. No spaces or other characters are allowed.',
	alphanum: 'Please use only letters (a-z) or numbers (0-9) in this field. No spaces or other characters are allowed.',
	dateSuchAs: 'Please enter a valid date such as {date}',
	dateInFormatMDY: 'Please enter a valid date such as MM/DD/YYYY (i.e. "12/31/1999")',
	email: 'Please enter a valid email address. For example "fred@domain.com".',
	url: 'Please enter a valid URL such as http://www.google.com.',
	currencyDollar: 'Please enter a valid $ amount. For example $100.00 .',
	oneRequired: 'Please enter something for at least one of these inputs.',
	errorPrefix: 'Error: ',
	warningPrefix: 'Warning: ',

	// Form.Validator.Extras
	noSpace: 'There can be no spaces in this input.',
	reqChkByNode: 'No items are selected.',
	requiredChk: 'This field is required.',
	reqChkByName: 'Please select a {label}.',
	match: 'This field needs to match the {matchName} field',
	startDate: 'the start date',
	endDate: 'the end date',
	currendDate: 'the current date',
	afterDate: 'The date should be the same or after {label}.',
	beforeDate: 'The date should be the same or before {label}.',
	startMonth: 'Please select a start month',
	sameMonth: 'These two dates must be in the same month - you must change one or the other.',
	creditcard: 'The credit card number entered is invalid. Please check the number and try again. {length} digits entered.'

});

/*
---

script: Fx.Scroll.js

name: Fx.Scroll

description: Effect to smoothly scroll any element, including the window.

license: MIT-style license

authors:
  - Valerio Proietti

requires:
  - Core/Fx
  - Core/Element.Event
  - Core/Element.Dimensions
  - /MooTools.More

provides: [Fx.Scroll]

...
*/

Fx.Scroll = new Class({

	Extends: Fx,

	options: {
		offset: {x: 0, y: 0},
		wheelStops: true
	},

	initialize: function(element, options){
		this.element = this.subject = document.id(element);
		this.parent(options);
		var cancel = this.cancel.bind(this, false);

		if ($type(this.element) != 'element') this.element = document.id(this.element.getDocument().body);

		var stopper = this.element;

		if (this.options.wheelStops){
			this.addEvent('start', function(){
				stopper.addEvent('mousewheel', cancel);
			}, true);
			this.addEvent('complete', function(){
				stopper.removeEvent('mousewheel', cancel);
			}, true);
		}
	},

	set: function(){
		var now = Array.flatten(arguments);
		if (Browser.Engine.gecko) now = [Math.round(now[0]), Math.round(now[1])];
		this.element.scrollTo(now[0] + this.options.offset.x, now[1] + this.options.offset.y);
	},

	compute: function(from, to, delta){
		return [0, 1].map(function(i){
			return Fx.compute(from[i], to[i], delta);
		});
	},

	start: function(x, y){
		if (!this.check(x, y)) return this;
		var scrollSize = this.element.getScrollSize(),
			scroll = this.element.getScroll(),
			values = {x: x, y: y};
		for (var z in values){
			var max = scrollSize[z];
			if ($chk(values[z])) values[z] = ($type(values[z]) == 'number') ? values[z] : max;
			else values[z] = scroll[z];
			values[z] += this.options.offset[z];
		}
		return this.parent([scroll.x, scroll.y], [values.x, values.y]);
	},

	toTop: function(){
		return this.start(false, 0);
	},

	toLeft: function(){
		return this.start(0, false);
	},

	toRight: function(){
		return this.start('right', false);
	},

	toBottom: function(){
		return this.start(false, 'bottom');
	},

	toElement: function(el){
		var position = document.id(el).getPosition(this.element);
		return this.start(position.x, position.y);
	},

	scrollIntoView: function(el, axes, offset){
		axes = axes ? $splat(axes) : ['x','y'];
		var to = {};
		el = document.id(el);
		var pos = el.getPosition(this.element);
		var size = el.getSize();
		var scroll = this.element.getScroll();
		var containerSize = this.element.getSize();
		var edge = {
			x: pos.x + size.x,
			y: pos.y + size.y
		};
		['x','y'].each(function(axis) {
			if (axes.contains(axis)) {
				if (edge[axis] > scroll[axis] + containerSize[axis]) to[axis] = edge[axis] - containerSize[axis];
				if (pos[axis] < scroll[axis]) to[axis] = pos[axis];
			}
			if (to[axis] == null) to[axis] = scroll[axis];
			if (offset && offset[axis]) to[axis] = to[axis] + offset[axis];
		}, this);
		if (to.x != scroll.x || to.y != scroll.y) this.start(to.x, to.y);
		return this;
	},

	scrollToCenter: function(el, axes, offset){
		axes = axes ? $splat(axes) : ['x', 'y'];
		el = $(el);
		var to = {},
			pos = el.getPosition(this.element),
			size = el.getSize(),
			scroll = this.element.getScroll(),
			containerSize = this.element.getSize(),
			edge = {
				x: pos.x + size.x,
				y: pos.y + size.y
			};

		['x','y'].each(function(axis){
			if(axes.contains(axis)){
				to[axis] = pos[axis] - (containerSize[axis] - size[axis])/2;
			}
			if(to[axis] == null) to[axis] = scroll[axis];
			if(offset && offset[axis]) to[axis] = to[axis] + offset[axis];
		}, this);
		if (to.x != scroll.x || to.y != scroll.y) this.start(to.x, to.y);
		return this;
	}

});
