// Arbitrary frontend code is loaded from this package after a restart.
window.dispatchEvent(new CustomEvent('revlm-plugin-loaded', { detail: { id: 'Anthropic' } }));
