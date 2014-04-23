---
---
/***
 * APE JSF Setup
 */

APE.Config.baseUrl = '{{site.APEJSF}}'; 
APE.Config.domain = 'auto';
APE.Config.server = '{{site.APEServer}}';

/***
 * APE CORE Files
 */

APE.Config.scripts.push(APE.Config.baseUrl + '/apeCore.min.js');
//APE.Config.scripts.push(APE.Config.baseUrl + '/apeCoreSession.min.js'); //Uncomment to enable Sessions
