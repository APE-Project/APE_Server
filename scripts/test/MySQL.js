Ape.log(' =====================================');
Ape.log('      Start up for test/MySQL.js      ');
Ape.log(' =====================================\n');


var server = "1.1.1.1:3306";
var sql = new Ape.MySQL(server, "ape", "ape", "ape_test");
Ape.log('[MySQL] Connecting to MySQL server : ' + server);

Ape.log('\n >>> Test for MySQL Support. You should see "----[object MySQL]----" next line:');
Ape.log(sql); //Spidermonkey 1.8.5 check

sql.onConnect = function() {
    Ape.log('[MySQL] Connected to mysql server. Trying Query');

    sql.query("SELECT * FROM test_table", function(res, errorNo) {
            if (errorNo) {
                    Ape.log('[MySQL] Request error : ' + errorNo + ' : '+ this.errorString());
            } else {
                    Ape.log('[MySQL] Fetching ' + res.length);
                    for(var i = 0; i < res.length; i++) {
                            Ape.log(res[i].ID + "  -> " + res[i].value); //res[i].<column name>
                    }
            }
    });
}

sql.onError = function(errorNo) {
  Ape.log('[MySQL] Connection Error : ' + errorNo + ' : '+ this.errorString());
}

Ape.log("\n\n");