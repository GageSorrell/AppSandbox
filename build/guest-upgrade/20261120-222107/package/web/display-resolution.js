/* Shared resolution value rules for the Windows VM editor. */
'use strict';
var DisplayResolution = (function() {
    var keys = ['windowedWidth', 'windowedHeight', 'fullscreenOverride', 'fullscreenWidth', 'fullscreenHeight'];
    function defaults() {
        return {windowedWidth:1920, windowedHeight:1080, fullscreenOverride:false, fullscreenWidth:1920, fullscreenHeight:1080};
    }
    function validPair(width, height) {
        return Number.isInteger(width) && width >= 640 && width <= 7680 &&
            Number.isInteger(height) && height >= 480 && height <= 4320;
    }
    function normalized(value) {
        var result = defaults();
        if (!value) return result;
        ['windowed', 'fullscreen'].forEach(function(kind) {
            if (validPair(value[kind + 'Width'], value[kind + 'Height'])) {
                result[kind + 'Width'] = value[kind + 'Width'];
                result[kind + 'Height'] = value[kind + 'Height'];
            }
        });
        result.fullscreenOverride = value.fullscreenOverride === true;
        return result;
    }
    function dimension(text) {
        return /^\d+$/.test(text) ? Number(text) : NaN;
    }
    function equal(a, b) {
        return !!a && !!b && keys.every(function(key) { return a[key] === b[key]; });
    }
    function warnings(width, height, capabilities) {
        var result = {limit:'', small:''};
        if (!validPair(width, height)) return result;
        if (capabilities && capabilities.ok && !capabilities.monitors.some(function(monitor) {
            return monitor.modes.some(function(mode) { return mode.width >= width && mode.height >= height; });
        })) result.limit = 'No connected monitor has a supported mode at least this large in both dimensions.';
        if (width < 1280 || height < 720)
            result.small = 'This resolution is below 1280×720. Windows 11 apps may not fit on screen.';
        return result;
    }
    return {defaults:defaults, validPair:validPair, normalized:normalized, dimension:dimension, equal:equal, warnings:warnings};
})();
if (typeof module !== 'undefined') module.exports = DisplayResolution;
