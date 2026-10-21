'use strict';
self.onmessage = async event => {
    try {
        const response = await fetch(event.data);
        if (!response.ok) throw new Error('worker transfer failed');
        self.postMessage({kind: 'worker-headers'});
        const reader = response.body.getReader();
        while (!(await reader.read()).done) { }
    } catch (error) {
        self.postMessage({error: String(error)});
    }
};
