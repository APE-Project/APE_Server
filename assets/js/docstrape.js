
//set the active menu
var active = document.getElementById('{+JSDOC.opt.D.topic+}');
if (active) active.className = 'active';


//http://stackoverflow.com/questions/7862233/twitter-bootstrap-tabs-go-to-specific-tab-on-page-reload/15060168#15060168
function handleTabLinks(url) {
	if (url) {
		var p = url.lastIndexOf('#');
		if (p != -1) {
			// Change hash for page-reload
			window.location.hash = url.substring(p);
		}
	}
	if (window.location.hash != '') {
		var makeActive = 'summary';
		var nestedActive = '';
		switch (window.location.hash) {
			case '#class_summary': makeActive = 'class_summary'; break;
			case '#constructor_details': makeActive = 'constructor_details'; break;
			case '#property_summary': //
			case '#property_details': makeActive = 'property_details'; nestedActive = 'properties_summary'; break;
			case '#methods_summary': //ft
			case '#methods_details': makeActive = 'methods_details'; nestedActive = 'methods_summary'; break;
			case '#events_summary': //ft
			case '#events_details': makeActive = 'events_details'; nestedActive = 'events_summary'; break;
			default:
				nestedActive = window.location.hash.substring(1);
				if (/static_event_/.test(window.location.hash)) {
					makeActive = 'events_details';
				} else if (/static_/.test(window.location.hash)) {
					makeActive = 'methods_details';
				} else {
					makeActive = 'property_details';
				}
		}
		var e = $('.nav-tabs a[href=#' + makeActive + ']');
		if (e) {
			e.tab('show');
		}
		e = $('.nav-tabs a[href*=#' + nestedActive + ']');
		if (e) {
			e.tab('show');
		}
		window.scrollTo(0, 0);
	}
}
handleTabLinks();
