// Pico Universal Pro — Parametric Snap-Fit Consumer Enclosure (OpenSCAD)
// Matches 4-layer PU_BOARD_PRO_PCB (55.0 mm x 24.0 mm x 1.6 mm)
// Material: ABS / ASA (3D-printable for Rev A/B pilots; 1.5 deg draft-angle ready for injection molding)

$fn = 48;

// PCB dimensions
pcb_len   = 55.0;
pcb_wid   = 24.0;
pcb_thk   = 1.6;
clearance = 0.35;

// Shell parameters
wall      = 1.6;
floor_thk = 1.6;
ceil_thk  = 1.6;
pcb_z     = floor_thk + 2.0; // bottom clearance for pogo pads / through-hole pins
inner_h   = 9.6;             // accommodates 2.54mm shrouded box header + USB-C

outer_len = pcb_len + 2 * (wall + clearance);
outer_wid = pcb_wid + 2 * (wall + clearance);
outer_h   = floor_thk + inner_h + ceil_thk;
corner_r  = 2.5;

// Cutout specifications
usbc_w    = 9.4;
usbc_h    = 3.6;
hdr254_w  = 14.2; // 10-pin 2.54mm keyed shrouded box header
hdr254_h  = 9.2;
hdr127_w  = 9.5;  // 10-pin 1.27mm ARM Cortex header
hdr127_h  = 5.2;
led_d     = 2.6;  // RGB light-pipe aperture
btn_d     = 2.0;  // Recessed Recovery/BOOTSEL pinhole

module rounded_box(l, w, h, r) {
    hull() {
        for (x = [r, l - r]) {
            for (y = [r, w - r]) {
                translate([x, y, 0])
                    cylinder(h = h, r = r);
            }
        }
    }
}

module bottom_shell() {
    split_z = pcb_z + pcb_thk + 1.2;
    difference() {
        // Outer lower body
        rounded_box(outer_len, outer_wid, split_z, corner_r);

        // Inner cavity
        translate([wall, wall, floor_thk])
            rounded_box(outer_len - 2*wall, outer_wid - 2*wall, split_z, max(1.0, corner_r - wall));

        // USB-C cutout on X=0 face
        translate([-1, (outer_wid - usbc_w)/2, pcb_z + pcb_thk - 0.3])
            cube([wall + 3, usbc_w, usbc_h]);

        // Universal 10-pin 2.54mm header cutout on X=outer_len face
        translate([outer_len - wall - 1, (outer_wid - hdr254_w)/2, pcb_z + pcb_thk - 0.5])
            cube([wall + 3, hdr254_w, hdr254_h]);

        // Recessed Recovery/BOOTSEL pinhole on side face (Y=0)
        translate([16.0, -1, pcb_z + pcb_thk + 1.2])
            rotate([-90, 0, 0])
                cylinder(h = wall + 3, d = btn_d);
    }

    // Four PCB support standoffs
    for (sx = [wall + 3.5, outer_len - wall - 3.5]) {
        for (sy = [wall + 3.0, outer_wid - wall - 3.0]) {
            translate([sx, sy, floor_thk])
                cylinder(h = pcb_z - floor_thk, d = 3.2);
        }
    }

    // Four snap-fit cantilever hooks along long edges
    for (cx = [outer_len * 0.28, outer_len * 0.72]) {
        for (cy = [wall + 0.1, outer_wid - wall - 0.9]) {
            translate([cx - 3.0, cy, split_z - 1.2])
                cube([6.0, 0.8, 2.4]);
        }
    }
}

module top_shell() {
    split_z = pcb_z + pcb_thk + 1.2;
    top_h   = outer_h - split_z;
    difference() {
        rounded_box(outer_len, outer_wid, top_h, corner_r);

        // Inner cavity
        translate([wall, wall, -0.5])
            rounded_box(outer_len - 2*wall, outer_wid - 2*wall, top_h - ceil_thk + 0.5, max(1.0, corner_r - wall));

        // Upper half of USB-C aperture
        translate([-1, (outer_wid - usbc_w)/2, -0.5])
            cube([wall + 3, usbc_w, 2.5]);

        // Upper half of 2.54mm shrouded header aperture
        translate([outer_len - wall - 1, (outer_wid - hdr254_w)/2, -0.5])
            cube([wall + 3, hdr254_w, hdr254_h]);

        // Top-access 10-pin 1.27mm ARM Cortex header aperture
        translate([outer_len - wall - 17.5, (outer_wid - hdr127_w)/2, top_h - ceil_thk - 1])
            cube([hdr127_h, hdr127_w, ceil_thk + 2]);

        // Diffused RGB status LED light-pipe aperture
        translate([12.0, outer_wid/2, top_h - ceil_thk - 1])
            cylinder(h = ceil_thk + 2, d = led_d);
    }
}

// Render both shells side-by-side for printing / CAD export
bottom_shell();
translate([0, outer_wid + 8.0, 0])
    top_shell();
