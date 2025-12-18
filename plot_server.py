from flask import Flask, jsonify, render_template_string
import pandas as pd
from pathlib import Path

app = Flask(__name__)

HTML_PAGE = """
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>PSD Dinámica en Tiempo Real</title>
    <script src="https://cdn.plot.ly/plotly-latest.min.js"></script>
    <style>
        body { font-family: Arial; background: #f5f6fa; text-align: center; }
        h1 { background: #1976d2; color: white; margin: 0; padding: 15px; }
        #chart { width: 90%; margin: 30px auto; }
        p { color: gray; font-size: 14px; }
    </style>
</head>
<body>
    <h1>PSD en Tiempo Real</h1>
    <div id="chart"></div>
    <p>Última actualización: <span id="time">--:--:--</span></p>

    <script>
        let chartInit = false;

        async function fetchData() {
            const response = await fetch('/data');
            const data = await response.json();
            return data;
        }

        async function updatePlot() {
            const data = await fetchData();

            const trace = {
                x: data.freq,
                y: data.psd,
                type: 'scatter',
                mode: 'lines',
                line: { color: '#1e88e5' }
            };

            const layout = {
                title: 'Densidad Espectral de Potencia (PSD)',
                xaxis: { title: 'Frecuencia [Hz]' },
                yaxis: { title: 'Potencia [dBFS]' },
                margin: { t: 50, l: 70, r: 30, b: 60 }
            };

            if (!chartInit) {
                Plotly.newPlot('chart', [trace], layout);
                chartInit = true;
            } else {
                Plotly.react('chart', [trace], layout);
            }

            document.getElementById('time').innerText = new Date().toLocaleTimeString();
        }

        updatePlot(); // primer render
        setInterval(updatePlot, 500); // actualiza cada 1s
    </script>
</body>
</html>
"""

@app.route('/')
def home():
    return render_template_string(HTML_PAGE)

@app.route('/data')
def get_data():
    csv_path = Path("static/last_psd.csv")

    if not csv_path.exists():
        return jsonify({"freq": [], "psd": []})

    try:
        df = pd.read_csv(csv_path)
        freq_col = df.columns[0]
        psd_col = df.columns[1]
        data = {
            "freq": df[freq_col].tolist(),
            "psd": df[psd_col].tolist()
        }
        return jsonify(data)
    except Exception as e:
        return jsonify({"error": str(e), "freq": [], "psd": []})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=False)
