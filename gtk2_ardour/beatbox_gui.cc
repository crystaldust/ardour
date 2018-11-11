/*
    Copyright (C) 2017 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <cstdlib>
#include <ctime>

#include "pbd/compose.h"
#include "pbd/i18n.h"

#include "ardour/beatbox.h"
#include "ardour/session.h"
#include "ardour/smf_source.h"
#include "ardour/source_factory.h"
#include "ardour/step_sequencer.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/utils.h"

#include "canvas/canvas.h"
#include "canvas/grid.h"
#include "canvas/box.h"
#include "canvas/rectangle.h"
#include "canvas/polygon.h"
#include "canvas/scroll_group.h"
#include "canvas/step_button.h"
#include "canvas/text.h"
#include "canvas/widget.h"

#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/utils.h"
#include "gtkmm2ext/colors.h"

#include "beatbox_gui.h"
#include "timers.h"
#include "ui_config.h"

using namespace PBD;
using namespace ARDOUR;
using namespace Gtkmm2ext;
using namespace ArdourCanvas;

using std::cerr;
using std::endl;

const int _nsteps = 32;
const int _nrows = 8;
const double _step_dimen = 32;

BBGUI::BBGUI (boost::shared_ptr<BeatBox> bb)
	: ArdourDialog (_("BeatBox"))
	, bbox (bb)
	, horizontal_adjustment (0.0, 0.0, 800.0)
	, vertical_adjustment (0.0, 0.0, 10.0, 400.0)
	, vscrollbar (vertical_adjustment)
	, mode_velocity_button (_("Velocity"))
	, mode_pitch_button (_("Pitch"))
	, mode_octave_button (_("Octave"))
	, mode_group_button (_("Group"))
	, mode_duration_button (_("Gate"))

{
	_canvas_viewport = new GtkCanvasViewport (horizontal_adjustment, vertical_adjustment);
	_canvas = _canvas_viewport->canvas();
	_canvas->set_background_color (UIConfiguration::instance().color ("gtk_bases"));
	_canvas->use_nsglview ();

	_sequencer = new SequencerGrid (bbox->sequencer(), _canvas);

	canvas_hbox.pack_start (*_canvas_viewport, true, true);
	canvas_hbox.pack_start (vscrollbar, false, false);

	get_vbox()->set_spacing (12);
	get_vbox()->pack_start (mode_box, false, false);
	get_vbox()->pack_start (canvas_hbox, true, true);

	export_as_region_button.signal_clicked.connect (sigc::mem_fun (*this, &BBGUI::export_as_region));
	get_action_area()->pack_end (export_as_region_button);

	bbox->sequencer().PropertyChanged.connect (sequencer_connection, invalidator (*this), boost::bind (&BBGUI::sequencer_changed, this, _1), gui_context());

	{
		/* trigger initial draw */
		PropertyChange pc;
		sequencer_changed (pc);
	}

	show_all ();
}

BBGUI::~BBGUI ()
{
}

void
BBGUI::mode_clicked (SequencerGrid::Mode m)
{
	_sequencer->set_mode (m);
}

void
BBGUI::update ()
{
	_sequencer->update ();
}

void
BBGUI::on_map ()
{
	timer_connection = Timers::rapid_connect (sigc::mem_fun (*this, &BBGUI::update));
	ArdourDialog::on_map ();
}

void
BBGUI::on_unmap ()
{
	timer_connection.disconnect ();
	ArdourDialog::on_unmap ();
}

#if 0
bool
BBGUI::SwitchRow::switch_event (GdkEvent* ev, int col)
{
	Timecode::BBT_Time at;

	const int beats_per_bar = owner.bbox->meter_beats();

	at.bars = col / beats_per_bar;
	at.beats = col % beats_per_bar;
	at.ticks = 0;

	at.bars++;
	at.beats++;

	Switch* s = switches[col];

	if (ev->type == GDK_BUTTON_PRESS) {
		/* XXX changes hould be driven by model */
		if (s->button->value()) {
			owner.bbox->remove_note (note, at);
			s->button->set_value (0);
		} else {
			s->button->set_value (64);
			owner.bbox->add_note (note, rint (s->button->value()), at);
		}
		return true;
	} else if (ev->type == GDK_SCROLL) {
		switch (ev->scroll.direction) {
		case GDK_SCROLL_UP:
		case GDK_SCROLL_RIGHT:
			s->button->set_value (s->button->value() + 1);
			break;
		case GDK_SCROLL_DOWN:
		case GDK_SCROLL_LEFT:
			s->button->set_value (s->button->value() - 1);
			break;
		}
		return true;
	}

	return false;
}
#endif

void
BBGUI::clear ()
{
}

void
BBGUI::export_as_region ()
{
	std::string path = bbox->session().new_midi_source_path (bbox->owner()->name());

	boost::shared_ptr<SMFSource> src;

	/* caller must check for pre-existing file */

	assert (!path.empty());
	assert (!Glib::file_test (path, Glib::FILE_TEST_EXISTS));

	src = boost::dynamic_pointer_cast<SMFSource>(SourceFactory::createWritable (DataType::MIDI, bbox->session(), path, false, bbox->session().sample_rate()));

	try {
		if (src->create (path)) {
			return;
		}
	} catch (...) {
		return;
	}

	if (!bbox->fill_source (src)) {

		return;
	}

	std::string region_name = region_name_from_path (src->name(), true);

	PBD::PropertyList plist;

	plist.add (ARDOUR::Properties::start, 0);
	plist.add (ARDOUR::Properties::length, src->length (0));
	plist.add (ARDOUR::Properties::name, region_name);
	plist.add (ARDOUR::Properties::layer, 0);
	plist.add (ARDOUR::Properties::whole_file, true);
	plist.add (ARDOUR::Properties::external, false);

	boost::shared_ptr<Region> region = RegionFactory::create (src, plist, true);
}

void
BBGUI::sequencer_changed (PropertyChange const &)
{
	const size_t nsteps = bbox->sequencer().step_capacity ();
	const size_t nsequences = bbox->sequencer().nsequences();

	_width = _step_dimen * nsteps;
	_height = _step_dimen * nsequences;

	/* height is 1 step_dimen larger to accomodate the "step indicator"
	 * line at the top
	 */

	_canvas_viewport->set_size_request (250.0 + _width, _height + _step_dimen);
}

/**/

SequencerGrid::SequencerGrid (StepSequencer& s, Canvas* c)
	: Rectangle (c)
	, _sequencer (s)
	, _mode (Velocity)
{
	no_scroll_group = new ArdourCanvas::Container (_canvas->root());
	step_indicator_box = new ArdourCanvas::Container (no_scroll_group);
	step_indicator_box->set_position (Duple (250.0, 70.0));

	v_scroll_group = new ScrollGroup (_canvas->root(), ScrollGroup::ScrollsVertically);
	_canvas->add_scroller (*v_scroll_group);
	v_scroll_group->add (this);

	ArdourCanvas::Text* label;

	velocity_mode_button = new Rectangle (no_scroll_group);
	velocity_mode_button->set_position (Duple (250.0 + 10, 10));
	velocity_mode_button->set (Rect (0, 0, 110, 60));
	velocity_mode_button->set_fill_color (UIConfiguration::instance().color ("gtk_bright_color"));
	label = new Text (velocity_mode_button);
	label->set_font_description (UIConfiguration::instance().get_NormalFont());
	label->set_position (Duple (30, 30));
	label->set (_("Vel"));
	label->set_color (contrasting_text_color (velocity_mode_button->fill_color()));

	pitch_mode_button = new Rectangle (no_scroll_group);
	pitch_mode_button->set_position (Duple (250.0 + 110, 10));
	pitch_mode_button->set (Rect (0, 0, 110, 60));
	pitch_mode_button->set_fill_color (UIConfiguration::instance().color ("gtk_bright_color"));
	label = new Text (pitch_mode_button);
	label->set_font_description (UIConfiguration::instance().get_NormalFont());
	label->set_position (Duple (30, 30));
	label->set (_("Pitch"));
	label->set_color (contrasting_text_color (pitch_mode_button->fill_color()));

	gate_mode_button = new Rectangle (no_scroll_group);
	gate_mode_button->set_position (Duple (250.0 + 220, 10));
	gate_mode_button->set (Rect (0, 0, 110, 60));
	gate_mode_button->set_fill_color (UIConfiguration::instance().color ("gtk_bright_color"));
	label = new Text (gate_mode_button);
	label->set_font_description (UIConfiguration::instance().get_NormalFont());
	label->set_position (Duple (30, 30));
	label->set (_("Gate"));
	label->set_color (contrasting_text_color (pitch_mode_button->fill_color()));

	octave_mode_button = new Rectangle (no_scroll_group);
	octave_mode_button->set_position (Duple (250.0 + 330, 10));
	octave_mode_button->set (Rect (0, 0, 110, 60));
	octave_mode_button->set_fill_color (UIConfiguration::instance().color ("gtk_bright_color"));
	label = new Text (octave_mode_button);
	label->set_font_description (UIConfiguration::instance().get_NormalFont());
	label->set_position (Duple (30, 30));
	label->set (_("Oct"));
	label->set_color (contrasting_text_color (pitch_mode_button->fill_color()));

	set_position (Duple (250, _step_dimen + 70.0));

	_sequencer.PropertyChanged.connect (sequencer_connection, invalidator (*this), boost::bind (&SequencerGrid::sequencer_changed, this, _1), gui_context());

	{
		/* trigger initial draw */
		PropertyChange pc;
		sequencer_changed (pc);
	}
}

void
SequencerGrid::update ()
{
	bool running = true;
	size_t step = _sequencer.last_step ();

	if (!running) {
		for (StepIndicators::iterator s = step_indicators.begin(); s != step_indicators.end(); ++s) {
			(*s)->set_current (false);
		}
	} else {
		size_t n = 0;
		for (StepIndicators::iterator s = step_indicators.begin(); s != step_indicators.end(); ++s, ++n) {
			if (n == step) {
				(*s)->set_current (true);
			} else {
				(*s)->set_current (false);
			}
		}
	}
}

void
SequencerGrid::sequencer_changed (PropertyChange const &)
{
	const size_t nsteps = _sequencer.step_capacity ();
	const size_t nsequences = _sequencer.nsequences();

	_width = _step_dimen * nsteps;
	_height = _step_dimen * nsequences;

	step_indicator_box->clear (true); /* delete all existing step indicators */
	step_indicators.clear ();

	step_indicator_bg = new ArdourCanvas::Rectangle (step_indicator_box);
	step_indicator_bg->set_fill_color (HSV (UIConfiguration::instance().color ("gtk_bases")).lighter (0.1));
	step_indicator_bg->set_outline (false);
	step_indicator_bg->set (Rect (0, 0, _width, _step_dimen));

	/* indicator row */

	for (size_t n = 0; n < nsteps; ++n) {
		SequencerStepIndicator* ssi = new SequencerStepIndicator (*this, step_indicator_box, n);
		ssi->set_position (Duple (n * _step_dimen, 0));
		ssi->set (Rect (0, 0, _step_dimen, _step_dimen));
		ssi->set_fill_color (random());
		step_indicators.push_back (ssi);
	}

	/* step views, one per step per sequence */

	clear (true);
	step_views.clear ();

	for (size_t s = 0; s < nsequences; ++s) {
		for (size_t n = 0; n < nsteps; ++n) {
			StepView* sv = new StepView (*this, _sequencer.sequence (s).step (n), v_scroll_group);
			/* sequence row is 1-row down ... because of the step indicator row */
			sv->set_position (Duple (250.0 + (n * _step_dimen), 70.0 + (s+1) * _step_dimen));
			sv->set (Rect (1, 1, _step_dimen - 2, _step_dimen - 2));
			step_views.push_back (sv);
			step_views.push_back (sv);
		}
	}
}

void
SequencerGrid::set_mode (Mode m)
{
	_mode = m;

	for (StepViews::iterator s = step_views.begin(); s != step_views.end(); ++s) {
		(*s)->view_mode_changed ();
	}

	redraw ();
}

void
SequencerGrid::render (Rect const & area, Cairo::RefPtr<Cairo::Context> context) const
{
	Rect self (item_to_window (get(), false));
	const Rect draw = self.intersection (area);

	if (!draw) {
		return;
	}

	setup_fill_context (context);
	context->rectangle (draw.x0, draw.y0, draw.width(), draw.height());
	context->fill ();

	context->set_line_width (1.0);

	/* horizontal lines */

	Gtkmm2ext::set_source_rgba (context, 0x000000ff);

	for (int n = 0; n < _nrows; ++n) {
		double x = 0;
		double y = n * _step_dimen;
		Duple start = item_to_window (Duple (x, y).translate (Duple (0.5, 0.5)));

		context->move_to (start.x, start.y);
		context->line_to (start.x + _width, start.y);
		context->stroke ();
	}

	/* vertical */

	for (int n = 0; n < _nsteps; ++n) {
		double x = n * _step_dimen;
		double y = 0;
		Duple start = item_to_window (Duple (x, y).translate (Duple (0.5, 0.5)));

		context->move_to (start.x, start.y);
		context->line_to (start.x, start.y + _height);
		context->stroke ();
	}

	render_children (area, context);
}

Gtkmm2ext::Color SequencerStepIndicator::other_color = Gtkmm2ext::Color (0);
Gtkmm2ext::Color SequencerStepIndicator::current_color = Gtkmm2ext::Color (0);
Gtkmm2ext::Color SequencerStepIndicator::other_text_color = Gtkmm2ext::Color (0);
Gtkmm2ext::Color SequencerStepIndicator::current_text_color = Gtkmm2ext::Color (0);
Gtkmm2ext::Color SequencerStepIndicator::bright_outline_color = Gtkmm2ext::Color (0);
int SequencerStepIndicator::dragging = 0;

SequencerStepIndicator::SequencerStepIndicator (SequencerGrid& s, Item *p, size_t n)
	: Rectangle (p)
	, grid (s)
	, number (n)
	, being_dragged (false)
{
	if (current_color == 0) { /* zero alpha? not set */
		other_color = UIConfiguration::instance().color ("gtk_bases");
		current_color = UIConfiguration::instance().color ("gtk_bright_color");
		other_text_color = contrasting_text_color (other_color);
		current_text_color = contrasting_text_color (current_color);
		bright_outline_color = UIConfiguration::instance().color ("gtk_bright_indicator");
	}

	set_fill (false);
	set_outline (false);

	poly = new Polygon (this);
	Points points;
	/* half pixel shifts are to get a clean single pixel outline */
	points.push_back (Duple (0.5, 0.5));
	points.push_back (Duple (_step_dimen - 0.5, 0.5));
	points.push_back (Duple (_step_dimen - 0.5, (_step_dimen - 1.0)/2.0));
	points.push_back (Duple ((_step_dimen - 1.0)/2.0, _step_dimen - 0.5));
	points.push_back (Duple (0.5, (_step_dimen - 1.0)/2.0));
	poly->set (points);
	poly->set_fill_color (current_color);
	poly->set_outline_color (other_color);
	poly->set_ignore_events (true);

	text = new Text (this);

	set_text ();

	text->set_font_description (UIConfiguration::instance ().get_NormalFont ());
	text->set_position (Duple ((_step_dimen/2.0) - (text->width()/2.0), 5.0));
	text->set_color (other_text_color);
	text->set_ignore_events (true);

	Event.connect (sigc::mem_fun (*this, &SequencerStepIndicator::on_event));
	grid.sequencer().PropertyChanged.connect (sequencer_connection, invalidator (*this), boost::bind (&SequencerStepIndicator::sequencer_changed, this, _1), gui_context());
}

void
SequencerStepIndicator::sequencer_changed (PropertyChange const &)
{
	set_text ();
}

void
SequencerStepIndicator::set_text ()
{
	if (number == grid.sequencer().end_step() - 1) {
		text->set ("\u21a9");
	} else if (number == grid.sequencer().start_step()) {
		text->set ("\u21aa");
	} else {
		text->set (string_compose ("%1", number+1));
	}
}

bool
SequencerStepIndicator::on_event (GdkEvent* ev)
{
	bool ret = false;

	switch (ev->type) {
	case GDK_ENTER_NOTIFY:
		switch (dragging) {
		case 1: /* end */
			poly->set_outline_color (bright_outline_color);
			poly->set_fill_color (current_color);
			being_dragged = true;
			break;
		case -1:
			poly->set_outline_color (bright_outline_color);
			poly->set_fill_color (current_color);
			being_dragged = true;
			break;
		default:
			poly->set_outline_color (other_color);
			poly->set_fill_color (other_color);
		}
		break;
	case GDK_LEAVE_NOTIFY:
		if (dragging) {
			poly->set_outline_color (other_color);
			poly->set_fill_color (other_color);
			being_dragged = false;
		}
		break;
	case GDK_MOTION_NOTIFY:
		ret = motion_event (&ev->motion);
		break;
	case GDK_BUTTON_PRESS:
		ret = button_press_event (&ev->button);
		break;
	case GDK_BUTTON_RELEASE:
		ret = button_release_event (&ev->button);
		break;
	default:
		break;
	}

	return ret;
}

bool
SequencerStepIndicator::motion_event (GdkEventMotion* ev)
{
	return true;
}

bool
SequencerStepIndicator::button_press_event (GdkEventButton* ev)
{
	if (number == grid.sequencer().end_step() - 1) {
		dragging = 1;
	} else if (number == grid.sequencer().start_step()) {
		dragging = -1;
	}

	return true;
}

bool
SequencerStepIndicator::button_release_event (GdkEventButton* ev)
{
	switch (dragging) {
	case 1:
		grid.sequencer().set_end_step (number+1);
		break;
	case -1:
		grid.sequencer().set_start_step (number);
		break;
	default:
		break;
	}

	dragging = 0;
	being_dragged = false;

	return true;
}

void
SequencerStepIndicator::set_current (bool yn)
{
	if (being_dragged) {
		return;
	}

	if (yn) {
		poly->set_fill_color (current_color);
		text->set_color (current_text_color);
	} else {
		poly->set_fill_color (other_color);
		text->set_color (other_text_color);
	}
}

void
SequencerStepIndicator::render  (Rect const & area, Cairo::RefPtr<Cairo::Context> context) const
{
	Rectangle::render (area, context);
	render_children (area, context);
}

Gtkmm2ext::Color StepView::on_fill_color = Gtkmm2ext::Color (0);
Gtkmm2ext::Color StepView::off_fill_color = Gtkmm2ext::Color (0);

StepView::StepView (SequencerGrid& sg, Step& s, ArdourCanvas::Item* parent)
	: ArdourCanvas::Rectangle (parent)
	, _step (s)
	, _seq (sg)
	, text (new Text (this))
	, grabbed (false)
{
	if (on_fill_color == 0) {
		on_fill_color = UIConfiguration::instance().color ("gtk_bases");
		off_fill_color = HSV (on_fill_color).lighter (0.1);
	}

	set_fill_color (off_fill_color);
	set_outline_color (UIConfiguration::instance().color ("gtk_bright_color"));
	set_outline (false);

	text->set_color (contrasting_text_color (fill_color()));
	text->set_font_description (UIConfiguration::instance ().get_SmallFont ());
	text->hide ();

	Event.connect (sigc::mem_fun (*this, &StepView::on_event));
	_step.PropertyChanged.connect (step_connection, invalidator (*this), boost::bind (&StepView::step_changed, this, _1), gui_context());
}

void
StepView::view_mode_changed ()
{
	/* this should leave the text to the last text-displaying mode */

	if (_seq.mode() == SequencerGrid::Octave) {
		set_octave_text ();
	} else if (_seq.mode() == SequencerGrid::Group) {
		set_group_text ();
	}
}

void
StepView::set_group_text ()
{
	text->set ("-");
}

void
StepView::set_octave_text ()
{
	if (_step.octave_shift() > 0) {
		text->set (string_compose ("+%1", _step.octave_shift()));
		text->show ();
	} else if (_step.octave_shift() == 0) {
		text->hide ();
	} else {
		text->set (string_compose ("%1", _step.octave_shift()));
		text->show ();
	}

	if (text->self_visible()) {
		const double w = text->width();
		const double h = text->height();
		text->set_position (Duple (_step_dimen/2 - (w/2), _step_dimen/2 - (h/2)));
	}
}

void
StepView::step_changed (PropertyChange const &)
{
	if (_seq.mode() == SequencerGrid::Octave) {
		set_octave_text ();
	}

	if (_step.velocity()) {
		set_fill_color (on_fill_color);
	} else {
		set_fill_color (off_fill_color);
	}

	redraw ();
}

void
StepView::render (ArdourCanvas::Rect const & area, Cairo::RefPtr<Cairo::Context> context) const
{
	Rectangle::render (area, context);

	if (_seq.mode() == SequencerGrid::Velocity) {
		if (_step.velocity()) {
			const double height = (_step_dimen - 4) * _step.velocity();
			const Duple origin = item_to_window (Duple (0, 0));
			set_source_rgba (context, outline_color());
			context->rectangle (origin.x + 2, origin.y + (_step_dimen - height - 2), _step_dimen - 4, height);
			context->fill ();
		}
	} else if (_seq.mode() == SequencerGrid::Pitch) {
		if (_step.velocity()) {
			const double height = (_step_dimen - 4) * (_step.note() / 128.0);
			const Duple origin = item_to_window (Duple (0, 0));
			set_source_rgba (context, outline_color());
			context->rectangle (origin.x + 2, origin.y + (_step_dimen - height - 2), _step_dimen - 4, height);
			context->fill ();
		}
	} else if (_seq.mode() == SequencerGrid::Duration) {
		if (_step.velocity()) {
			const Step::DurationRatio d (_step.duration());
			const double height = ((_step_dimen - 4.0) * d.numerator()) / d.denominator();
			const Duple origin = item_to_window (Duple (0, 0));
			set_source_rgba (context, outline_color());
			context->rectangle (origin.x + 2, origin.y + (_step_dimen - height - 2), _step_dimen - 4, height);
			context->fill ();
		}
	}

	/* now deal with any children (e.g. text) */

	render_children (area, context);
}

bool
StepView::on_event (GdkEvent *ev)
{
	bool ret = false;

	switch (ev->type) {
	case GDK_MOTION_NOTIFY:
		ret = motion_event (&ev->motion);
		break;
	case GDK_BUTTON_PRESS:
		ret = button_press_event (&ev->button);
		break;
	case GDK_BUTTON_RELEASE:
		ret = button_release_event (&ev->button);
		break;
	case GDK_SCROLL:
		ret = scroll_event (&ev->scroll);
	default:
		break;
	}

	return ret;
}


bool
StepView::motion_event (GdkEventMotion* ev)
{
	if (!grabbed) {
		return false;
	}

	const double distance = last_motion.second - ev->y;

	if ((ev->state & GDK_MOD1_MASK) || _seq.mode() == SequencerGrid::Pitch) {
		adjust_step_pitch (distance);
	} else if (_seq.mode() == SequencerGrid::Velocity) {
		adjust_step_velocity (distance);
	} else if (_seq.mode() == SequencerGrid::Duration) {
		adjust_step_duration (Step::DurationRatio (distance, 32)); /* adjust by 1/32 of the sequencer step size */
	} else if (_seq.mode() == SequencerGrid::Octave) {
		adjust_step_octave (distance);
	} else if (_seq.mode() == SequencerGrid::Group) {
	}

	last_motion = std::make_pair (ev->x, ev->y);
	return true;
}

bool
StepView::button_press_event (GdkEventButton* ev)
{
	grab_at = std::make_pair (ev->x, ev->y);
	last_motion = grab_at;
	grab ();
	grabbed = true;

	return true;
}

bool
StepView::button_release_event (GdkEventButton* ev)
{
	if (grabbed) {
		ungrab ();
		grabbed = false;

		if (fabs (last_motion.second - grab_at.second) < 4) {
			/* just a click */

			/* in all modes except octave, turn step on if it is off */

			if (_seq.mode() == SequencerGrid::Octave) {
				if (_step.velocity()) {
					_step.set_octave_shift (0);
				} else {
					_step.set_velocity (0.8);
				}
			} else {
				if (_step.velocity()) {
					_step.set_velocity (0.0);
				} else {
					_step.set_velocity (0.8);
				}
			}
		}
	}

	return true;
}

bool
StepView::scroll_event (GdkEventScroll* ev)
{
	int amt = 0;

	switch (ev->direction) {
	case GDK_SCROLL_UP:
		amt = 1;
		break;
	case GDK_SCROLL_LEFT:
		amt = -1;
		break;
	case GDK_SCROLL_RIGHT:
		amt = 1;
		break;
	case GDK_SCROLL_DOWN:
		amt = -1;
		break;
	}

	if ((ev->state & GDK_MOD1_MASK) || _seq.mode() == SequencerGrid::Pitch) {
		adjust_step_pitch (amt);
	} else if (_seq.mode() == SequencerGrid::Velocity) {
		adjust_step_velocity (amt);
	} else if (_seq.mode() == SequencerGrid::Duration) {
		/* adjust by 1/32 of the sequencer step size */
		adjust_step_duration (Step::DurationRatio (amt, 32));
	} else if (_seq.mode() == SequencerGrid::Octave) {
		adjust_step_octave (amt);
	} else if (_seq.mode() == SequencerGrid::Group) {
	}

	return true;
}

void
StepView::adjust_step_pitch (int amt)
{
	_step.adjust_pitch (amt);
}

void
StepView::adjust_step_velocity (int amt)
{
	_step.adjust_velocity (amt);
}

void
StepView::adjust_step_octave (int amt)
{
	_step.adjust_octave (amt);
}

void
StepView::adjust_step_duration (Step::DurationRatio const & amt)
{
	_step.adjust_duration (amt);
}
