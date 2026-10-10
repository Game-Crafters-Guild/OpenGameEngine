// The model viewer's Animation section: the shown model's clips, the playback speed and a pause
// button, with the playing clip for the panel's title, where a closed panel shows it. A model
// without clips leaves the section disabled, with a note in place of the clip list.
// It calls back into the page (main.js and panel.js) and never into the engine.

/**
 * @param {Document} page
 * @param {{ play: (clip: string, speed: number) => void, pause: () => void, setTitle: (title: string) => void }} callbacks
 *     `setTitle` receives the playback as the panel's title shows it.
 */
export function animationSection(page, { play, pause, setTitle }) {
    const field = (/** @type {string} */ name) => /** @type {any} */ (page.querySelector(`.panel [name=${name}]`));
    const [section, list, slider, speedOutput, pauseButton] = ['animation', 'clip', 'speed', 'rate', 'pause'].map(field);
    const none = /** @type {HTMLElement} */ (page.querySelector('.panel .no-animation'));
    const clipRow = /** @type {HTMLElement} */ (list.closest('label'));
    let paused = false;
    /** The playback as the panel's title shows it: '· Run', '· Run, paused', or '' without clips. */
    let title = '';
    const apply = () => {
        const speed = Number(slider.value);
        speedOutput.value = `${speed.toFixed(2)}×`;
        pauseButton.textContent = paused ? 'Play' : 'Pause';
        title = section.disabled ? '' : `· ${paused ? `${list.value}, paused` : list.value}`;
        setTitle(title);
        if (!section.disabled && !paused) play(list.value, speed);
    };

    list.addEventListener('change', () => { paused = false; apply(); });
    slider.addEventListener('input', apply);
    pauseButton.addEventListener('click', () => {
        paused = !paused;
        if (paused) pause();
        apply();
    });
    speedOutput.value = `${Number(slider.value).toFixed(2)}×`;

    return {
        /** Lists the clips of the model now on screen and plays the first. @param {string[]} clips */
        show: (clips) => {
            list.replaceChildren(...clips.map((clip) => new Option(clip, clip)));
            section.disabled = clips.length === 0;
            none.hidden = clips.length > 0;
            clipRow.hidden = clips.length === 0;
            paused = false;
            apply();
        },
        /** What the panel's title shows once a download is over. */
        title: () => title,
    };
}
