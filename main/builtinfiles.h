/**
 * @file builtinfiles.h
 * @brief This file is part of the WebServer example for the ESP8266WebServer.
 *  
 * This file contains long, multiline text variables for  all builtin resources.
 */


// used for $upload.htm
static const char uploadContent[] =
R"==(
<!doctype html>
<html lang='en'>

<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Upload</title>
</head>

<body style="width:300px">
  <h1>Upload</h1>
  <div><a href="/">Home</a></div>
  <hr>
  <div id='zone' style='width:16em;height:12em;padding:10px;background-color:#ddd'>Drop files here...</div>

  <script>
    // allow drag&drop of file objects 
    function dragHelper(e) {
      e.stopPropagation();
      e.preventDefault();
    }

    // allow drag&drop of file objects 
    function dropped(e) {
      dragHelper(e);
      var fls = e.dataTransfer.files;
      var formData = new FormData();
      for (var i = 0; i < fls.length; i++) {
        formData.append('file', fls[i], '/' + fls[i].name);
      }
      fetch('/', { method: 'POST', body: formData }).then(function () {
        window.alert('done.');
      });
    }
    var z = document.getElementById('zone');
    z.addEventListener('dragenter', dragHelper, false);
    z.addEventListener('dragover', dragHelper, false);
    z.addEventListener('drop', dropped, false);
  </script>
</body>
)==";

/*
 * if the url starts is http://xxx.xxx.xxx.xxx/$delete and it's a
 * HTTP_GET method, then send the html/js and it will be executed
 * by the browser given the Content-Type specified as the second
 * argument to the server.send() (seems that HTTP_GET is the default
 * if one types something in the address line of a browser)
 *
 * NOTES: 
 * 1) forms cannot use arbitrary methods like "HTTP_DELETE";
 *       need to use js which can.
 * 2) forms/browser have the default action of creating something
 *    like this by default:
 *    GET http://192.168.1.37/$delete?name=neo_user_1.json
 *    event.preventDefault() enables customization of action.
 * 3) it seems that javascript? browser? server code? understands
 *    "DELETE" to be "HTTP_DELETE" method
 * 4) even though the above handle() code seems to add the '/' to the
 *    start of the filename if it's missing, including the '/' makes
 *    the delete fail
 * 5) the above handle() code just expects the filename to be deleted,
 *    sent with the HTTP_DELETE method ... nothing fancy.
 * 6) the R"== is a raw string literal delimiter and allows for , 
 *    which allows you to include special characters like double quotes 
 *    within the string without needing escape sequences.  
 */
static const char deleteContent[] = R"==(
  <script>
    function deleteFile(event) {
      event.preventDefault();
      let filename = document.getElementById("filename").value;
      console.log(filename);
      fetch('/' + filename, { 
        method: "DELETE"
      })
      .then(response => response.text())
      .then(data => alert("Server response: " + data))
      .catch(error => console.error("Error:", error));
    }
  </script>
  <form onsubmit="deleteFile(event)">
    <label for="filename">Delete File</label>
    <input type="text" id="filename" name="name" placeholder="Enter filename(no leading /)"><br><br>
    <button type="submit">Delete</button>
  </form>
)==";

// used for $upload.htm
static const char notFoundContent[] = R"==(
<html>
<head>
  <title>Resource not found</title>
</head>
<body>
  <p>The resource was not found.</p>
  <p><a href="/">Start again</a></p>
</body>
)==";
