window.executeCommand = async (command, data = null) => {
    const response = await fetch('/command', {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: JSON.stringify({ command, data })
    });
    if (!response.ok) throw new Error('Launcher command failed (' + response.status + ').');
    return response.json();
};
