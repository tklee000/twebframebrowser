(function () {
    var nodes = document.querySelectorAll('[data-probe]');
    var boxes = [];
    for (var i = 0; i < nodes.length; i++) {
        var node = nodes[i];
        var rect = node.getBoundingClientRect();
        var style = getComputedStyle(node);
        boxes.push({id: node.id, rect: [rect.x, rect.y, rect.width, rect.height],
            display: style.display, position: style.position,
            fontSize: style.fontSize, fontFamily: style.fontFamily,
            writingMode: style.writingMode, opacity: style.opacity});
    }
    return JSON.stringify({viewport: [innerWidth, innerHeight],
        dpr: devicePixelRatio, elementCount: document.querySelectorAll('*').length,
        scripts: document.scripts.length, boxes: boxes});
}())
