// This file runs inside the real control-plane page after its own bundle.
// It may replace the DOM, attach routes, ship a second React tree, or do
// nothing. Revlm deliberately does not impose a frontend extension API.
window.dispatchEvent(new CustomEvent('revlm-plugin-loaded', { detail: { id: 'OpenAI' } }));
