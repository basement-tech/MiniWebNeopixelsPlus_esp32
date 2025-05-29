/**
 * Handles button click events by sending button data to the server API
 * 
 * @param {HTMLElement} targetElement - The button element that was clicked
 * @description 
 * This function takes a button element, extracts its value and data-file attributes,
 * and sends them to the server via a POST request to /api/button.
 * The data is sent as JSON in the format:
 * {
 *   sequence: <button value>,
 *   file: <data-file attribute value>
 * }
 * 
 * On success (status 201), the server response is logged to console.
 * On error, an alert is shown to the user and the error is logged.
 */
function callCFunction(targetElement)  {
  let seq_data = {
    sequence: String(targetElement.value),
    file: String(targetElement.dataset.file)
  }
  let jsonData = JSON.stringify(seq_data);
  console.log(jsonData)  // this works ... how to get to c callback in server.on?
  fetch('/api/button', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: jsonData
  })
  .then(response => {
    if(response.status != 201) {
      window.alert("Error processing button press");
    }
    return response.text();
  })
  .then(result => console.log(result))
  .catch(error => console.error('Error:', error));
}