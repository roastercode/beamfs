# SPDX-License-Identifier: GPL-2.0-only
#
# beamfs v3 paper: 3D figures from data/derived/*.csv (written by
# scripts/extract.py, which refuses to write anything when one of its
# cross-checks fails). Vector PDF, Latin Modern when TeX Live has it.
#
#   multifs-3d.pdf  run 4: filesystem x probability x flips received,
#                   coloured by outcome (one column wide; runs 2 and 3
#                   are in the replication table)
#   cluster-3d.pdf  runs 3 and 4: node x probability x flips received
#   rsmap-3d.pdf    beamfs, 1 000 000 ppm, runs 3 and 4: data block x
#                   codeword x distinct symbols hit over the attack,
#                   under the RS correction budget of 8 symbols
#
# Run from the workspace root: julia scripts/figures.jl

using CairoMakie

const W = dirname(@__DIR__)
const DER = joinpath(W, "data", "derived")
const FIG = joinpath(W, "figures")
mkpath(FIG)

function readcsv(path)
    lines = readlines(path)
    hdr = String.(split(lines[1], ','))
    rows = Dict{String,String}[]
    for ln in lines[2:end]
        isempty(ln) && continue
        f = String[]
        buf = IOBuffer()
        q = false
        for c in ln
            if c == '"'
                q = !q
            elseif c == ',' && !q
                push!(f, String(take!(buf)))
            else
                print(buf, c)
            end
        end
        push!(f, String(take!(buf)))
        length(f) == length(hdr) ||
            error("$(path) : $(length(f)) champs au lieu de $(length(hdr))")
        push!(rows, Dict(zip(hdr, f)))
    end
    rows
end

intval(s) = isempty(s) ? 0 : parse(Int, s)

function lmfont(name)
    try
        # readchomp gives a SubString, which Makie will not take as a font.
        p = String(readchomp(`kpsewhich $(name)`))
        return isfile(p) ? p : nothing
    catch
        return nothing
    end
end
const LMR = lmfont("lmroman10-regular.otf")
const LMB = lmfont("lmroman10-bold.otf")
const LMI = lmfont("lmroman10-italic.otf")
if LMR !== nothing && LMB !== nothing && LMI !== nothing
    set_theme!(Theme(fontsize = 16,
                     fonts = (; regular = LMR, bold = LMB, italic = LMI,
                              bold_italic = LMB)))
    println("police : Latin Modern ($(LMR))")
else
    set_theme!(Theme(fontsize = 16))
    println("police : defaut Makie (Latin Modern introuvable par kpsewhich)")
end

# Outcomes. Okabe-Ito hues for the three that carry meaning (checked
# for colour-vision deficiency); neutral greys for "nothing happened"
# and "not exercised", which are absences, not categories. Every bar
# also carries its flip count, so identity never rests on colour alone.
# The counts are plain black text: a white halo stroked by Cairo over
# glyphs this small covered them entirely (figures-v3-3).
const OUTCOMES = [
    ("intact", "intact (corrected)", "#009E73"),
    ("silent_corruption", "silent corruption", "#D55E00"),
    ("read_refused", "read refused (fail-closed)", "#E69F00"),
    ("no_flip", "no flip received", "#B8B8B8"),
    ("not_exercised", "not exercised (n/e)", "#F0F0F0"),
]
const OCOL = Dict(k => Makie.to_color(c) for (k, _, c) in OUTCOMES)
legend_elems() = [PolyElement(color = Makie.to_color(c), strokecolor = :black,
                              strokewidth = 0.5) for (_, _, c) in OUTCOMES]
legend_labels() = [l for (_, l, _) in OUTCOMES]

const PROBS = [1000, 100000, 1000000]
const PLAB = ["1 000", "100 000", "1 000 000"]
zlog(n) = log10(1 + n)
const ZT = (zlog.([0, 1, 10, 100, 1000]), ["0", "1", "10", "100", "1000"])
const BOX = Rect3f(Vec3f(-0.5, -0.5, 0), Vec3f(1, 1, 1))

function bars!(ax, xs, ys, ns, outs; w = 0.5)
    pos = [Point3f(x, y, 0) for (x, y) in zip(xs, ys)]
    hs = [max(zlog(n), 0.03) for n in ns]
    meshscatter!(ax, pos; marker = BOX,
                 markersize = [Vec3f(w, w, h) for h in hs],
                 color = [OCOL[o] for o in outs])
    lab = [o == "not_exercised" ? "n/e" : string(n) for (n, o) in zip(ns, outs)]
    text!(ax, [Point3f(x, y, h + 0.05) for (x, y, h) in zip(xs, ys, hs)];
          text = lab, align = (:center, :bottom), fontsize = 14,
          color = :black)
    length(pos)
end

function collect_cases(rows, run, key, cats)
    xs, ys, ns, os = Int[], Int[], Int[], String[]
    for r in rows
        r["run"] == run || continue
        push!(xs, findfirst(==(r[key]), cats))
        push!(ys, findfirst(==(parse(Int, r["prob_ppm"])), PROBS))
        push!(ns, intval(r["flip_delta"]))
        push!(os, r["outcome"])
    end
    xs, ys, ns, os
end

report = String[]

# ------------------------------------------------------------- multifs
let rows = readcsv(joinpath(DER, "multifs.csv"))
    fss = ["beamfs", "ext4", "ext3", "btrfs", "squashfs"]
    fig = Figure(size = (560, 560), figure_padding = (22, 22, 8, 26))
    ax = Axis3(fig[1, 1]; xlabel = "", ylabel = "ppm",
               zlabel = "flips received", azimuth = 1.38pi,
               elevation = 0.17pi, protrusions = (50, 20, 30, 30),
               xticks = (1:5, fss), yticks = (1:3, PLAB), zticks = ZT,
               limits = (0.5, 5.5, 0.5, 3.5, 0, 3.3),
               ylabeloffset = 70, zlabeloffset = 45)
    xs, ys, ns, os = collect_cases(rows, "run4", "fs", fss)
    length(xs) == 15 || error("multifs run4 : $(length(xs)) cases au lieu de 15")
    nb = bars!(ax, xs, ys, ns, os)
    Legend(fig[2, 1], legend_elems(), legend_labels(); nbanks = 2,
           orientation = :horizontal, framevisible = false, labelsize = 14)
    rowgap!(fig.layout, 2)
    p = joinpath(FIG, "multifs-3d.pdf")
    save(p, fig; pt_per_unit = 0.45)
    push!(report, "multifs-3d.pdf : $(nb) barres, $(filesize(p)) octets")
end

# ------------------------------------------------------------- cluster
let rows = readcsv(joinpath(DER, "cluster.csv"))
    nodes = ["beamfs-master", "beamfs-compute01", "beamfs-compute02",
             "beamfs-compute03"]
    nlab = ["master", "compute01", "compute02", "compute03"]
    fig = Figure(size = (1120, 520), figure_padding = (22, 22, 8, 26))
    nb = 0
    for (i, run) in enumerate(["run3", "run4"])
        ax = Axis3(fig[1, i]; title = run == "run4" ? "run 4 (reference)" : "run 3",
                   xlabel = "", ylabel = "ppm",
                   zlabel = "flips received", azimuth = 1.38pi,
                   elevation = 0.17pi, protrusions = (50, 20, 30, 30),
                   xticks = (1:4, nlab), yticks = (1:3, PLAB), zticks = ZT,
                   limits = (0.5, 4.5, 0.5, 3.5, 0, 3.3),
                   ylabeloffset = 70, zlabeloffset = 45)
        xs, ys, ns, os = collect_cases(rows, run, "node", nodes)
        length(xs) == 12 || error("cluster $(run) : $(length(xs)) cases au lieu de 12")
        nb += bars!(ax, xs, ys, ns, os)
    end
    colgap!(fig.layout, 30)
    Legend(fig[2, 1:2], legend_elems(), legend_labels();
           orientation = :horizontal, framevisible = false, labelsize = 14)
    rowgap!(fig.layout, 2)
    p = joinpath(FIG, "cluster-3d.pdf")
    save(p, fig; pt_per_unit = 0.45)
    push!(report, "cluster-3d.pdf : $(nb) barres, $(filesize(p)) octets")
end

# ------------------------------------------------------------- RS map
# One hue, light to dark, for a count of 1 to 3 (sequential, not a
# rainbow). The budget is drawn first, faint, so every column shows
# through it, and outlined so its height reads at a glance.
const SEQ = Dict(1 => Makie.to_color("#9ECAE1"), 2 => Makie.to_color("#3182BD"),
                 3 => Makie.to_color("#08306B"))
let rows = readcsv(joinpath(DER, "rs_margin.csv"))
    fig = Figure(size = (1180, 560), figure_padding = (22, 22, 8, 26))
    nb = 0
    hmax = 0
    for (i, run) in enumerate(["run3", "run4"])
        ax = Axis3(fig[1, i]; title = run == "run4" ? "run 4, 1 000 000 ppm" : "run 3, 1 000 000 ppm",
                   xlabel = "data block (logical)", ylabel = "codeword",
                   zlabel = "distinct symbols hit", azimuth = 1.30pi,
                   elevation = 0.14pi, aspect = (2.4, 1, 1),
                   protrusions = (50, 20, 30, 30), zlabeloffset = 40,
                   yticks = (0:5:15, string.(0:5:15)), zticks = 0:2:8,
                   limits = (-1, 69, -1, 16, 0, 9))
        mesh!(ax, Rect3f(Vec3f(-1, -1, 8), Vec3f(70, 17, 0.01));
              color = (:firebrick, 0.08))
        pos, hs = Point3f[], Int[]
        for r in rows
            (r["run"] == run && r["prob_ppm"] == "1000000") || continue
            push!(pos, Point3f(parse(Int, r["iblock"]), parse(Int, r["codeword"]), 0))
            push!(hs, parse(Int, r["symbols_hit"]))
        end
        isempty(pos) && error("rs_margin $(run) : aucune ligne a 1 000 000 ppm")
        maximum(hs) <= 8 || error("rs_margin $(run) : $(maximum(hs)) symboles > 8")
        hmax = max(hmax, maximum(hs))
        meshscatter!(ax, pos; marker = BOX,
                     markersize = [Vec3f(0.8, 0.7, h) for h in hs],
                     color = [SEQ[min(h, 3)] for h in hs], rasterize = 4)
        lines!(ax, [Point3f(-1, -1, 8), Point3f(69, -1, 8), Point3f(69, 16, 8),
                    Point3f(-1, 16, 8), Point3f(-1, -1, 8)];
               color = :firebrick, linewidth = 1.5, linestyle = :dash)
        text!(ax, Point3f(-1, 16, 8.2); text = "correction budget: 8 symbols per codeword",
              align = (:left, :bottom), fontsize = 13, color = :firebrick)
        nb += length(pos)
    end
    hmax <= 3 || error("rs_margin : $(hmax) symboles, la legende n'en prevoit que 3")
    Legend(fig[2, 1:2],
           [PolyElement(color = SEQ[k], strokecolor = :black, strokewidth = 0.5) for k in 1:3],
           ["1 symbol", "2 symbols", "3 symbols"],
           "distinct symbols hit in one codeword, union over the whole attack:";
           orientation = :horizontal, framevisible = false, labelsize = 14,
           titleposition = :left, titlesize = 14, titlefont = :regular)
    colgap!(fig.layout, 30)
    rowgap!(fig.layout, 2)
    p = joinpath(FIG, "rsmap-3d.pdf")
    save(p, fig; pt_per_unit = 0.45)
    push!(report, "rsmap-3d.pdf : $(nb) colonnes, max $(hmax) symboles, $(filesize(p)) octets")
end

for s in report
    println(s)
end
