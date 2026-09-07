# SPDX-License-Identifier: GPL-2.0-or-later
"""Native Qt interface; all changes originate from explicit user actions."""
import argparse
import json
import sys
from pathlib import Path
from PySide6.QtCore import Qt, QPointF, QProcess, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import (QApplication,QMainWindow,QWidget,QVBoxLayout,QHBoxLayout,
    QLabel,QPushButton,QComboBox,QCheckBox,QSpinBox,QScrollArea,QStackedWidget,
    QListWidget,QFileDialog,QMessageBox,QFrame,QSlider)
from . import backend, __version__

STYLE='''
QWidget { background:#101216; color:#e9edf3; font-family:"Noto Sans"; font-size:13px; }
QMainWindow { background:#101216; }
QListWidget { background:#171a20; border:0; min-width:180px; max-width:210px; padding:16px 8px; }
QListWidget::item { padding:13px 15px; border-radius:5px; }
QListWidget::item:selected { background:#33303a; color:#ff806e; }
QPushButton { background:#292e38; padding:8px 14px; border:1px solid #414855; border-radius:5px; }
QPushButton:hover { border-color:#ff806e; }
QPushButton:disabled, QComboBox:disabled, QSpinBox:disabled { color:#747c8a; }
QComboBox,QSpinBox { background:#20252d; border:1px solid #414855; padding:5px; }
QScrollArea { border:0; }
QLabel#title { font-size:28px; font-weight:600; }
QLabel#muted { color:#ff806e; font-weight:600; }
QLabel#hint { color:#aab2c0; }
QFrame#row { background:#191d24; border-radius:6px; }
QFrame#row QLabel { background:transparent; }
QSlider::groove:horizontal { height:4px; background:#414855; }
QSlider::handle:horizontal { background:#ff806e; width:13px; margin:-5px 0; border-radius:6px; }
'''

class Curve(QWidget):
    changed=Signal(list)
    def __init__(self,bands,editable):
        super().__init__();self.bands=bands;self.editable=editable;self.drag=None
        self.values=[int(c['values'][0]) for c in bands];self.setMinimumHeight(240)
        self.setToolTip('Drag a point to adjust its band; release to apply. The line shows settings, not a measured response.')
    def point(self,i):
        c=self.bands[i];ratio=(self.values[i]-c['min'])/max(1,c['max']-c['min'])
        return QPointF(40+i*(self.width()-80)/max(1,len(self.bands)-1),30+(1-ratio)*(self.height()-70))
    def paintEvent(self,event):
        p=QPainter(self);p.setRenderHint(QPainter.Antialiasing)
        p.setPen(QPen(QColor('#343b47'),1))
        for fraction in (0,.25,.5,.75,1):
            y=int(30+fraction*(self.height()-70));p.drawLine(40,y,self.width()-40,y)
        p.setPen(QPen(QColor('#ff806e'),2))
        points=QPolygonF([self.point(i) for i in range(len(self.bands))]);p.drawPolyline(points)
        for i,pt in enumerate(points):
            p.setBrush(QColor('#ff806e'));p.drawEllipse(pt,5,5)
            p.drawText(int(pt.x()-14),self.height()-15,str(i+1))
        p.setPen(QColor('#aab2c0'));p.drawText(4,18,'dB');p.drawText(40,18,'EQ bands 1–10 · setting curve')
    def mousePressEvent(self,event):
        if not self.editable or not self.bands:return
        self.drag=min(range(len(self.bands)),key=lambda i:abs(self.point(i).x()-event.position().x()))
        self.mouseMoveEvent(event)
    def mouseMoveEvent(self,event):
        if self.drag is None:return
        c=self.bands[self.drag];ratio=1-(event.position().y()-30)/max(1,self.height()-70)
        self.values[self.drag]=round(c['min']+min(1,max(0,ratio))*(c['max']-c['min']));self.update()
    def mouseReleaseEvent(self,event):
        if self.drag is not None:
            i=self.drag;self.drag=None;self.changed.emit([i,self.values[i]])

def hint(text):
    w=QLabel(text);w.setWordWrap(True);w.setObjectName('hint');return w

def group(c):
    n=c['name']
    if 'EQ Band' in n or 'Equalizer' in n:return 'Equalizer'
    if 'Capture' in n or n=='Input Source':return 'Recording'
    if n.startswith('FX:') or n.startswith('Enable OutFX'):return 'Acoustic effects'
    if n in backend.PROTECTED or n.startswith('AE-5:') or n.startswith('Bass Redirection') or n.startswith('Full-Range'):
        return 'Playback'
    return 'Mixer'

class Window(QMainWindow):
    def __init__(self,snapshot=None):
        super().__init__();self.snapshot=snapshot;self.card=None;self.cs=[];self.hw={};self.process=None
        self.setWindowTitle('AegAudio — Linux audio controls');self.resize(1150,780)
        root=QWidget();self.setCentralWidget(root);outer=QVBoxLayout(root)
        top=QHBoxLayout();brand=QLabel('AegAudio');brand.setObjectName('title');top.addWidget(brand);top.addStretch()
        self.devices=QComboBox();top.addWidget(self.devices)
        refresh=QPushButton('Refresh');refresh.clicked.connect(self.refresh);top.addWidget(refresh);outer.addLayout(top)
        self.banner=QLabel();self.banner.setWordWrap(True);outer.addWidget(self.banner)
        middle=QHBoxLayout();self.nav=QListWidget();self.pages=QStackedWidget()
        middle.addWidget(self.nav);middle.addWidget(self.pages,1);outer.addLayout(middle,1)
        self.footer=hint('No audio settings are changed when this application opens.');outer.addWidget(self.footer)
        self.nav.currentRowChanged.connect(self.pages.setCurrentIndex)
        try:self.inventory=snapshot['cards'] if snapshot else backend.cards()
        except Exception as exc:self.inventory=[];self.error(exc)
        for c in self.inventory:self.devices.addItem(c['name']+' · '+c['id'],c)
        selected=next((i for i,c in enumerate(self.inventory) if c.get('vendor')=='0x11020011'),0)
        self.devices.setCurrentIndex(selected);self.devices.currentIndexChanged.connect(self.refresh);self.refresh()
    def error(self,exc):QMessageBox.warning(self,'AegAudio',str(exc))
    def refresh(self):
        self.card=self.devices.currentData()
        if not self.card:
            self.banner.setText('No ALSA cards found. AegAudio needs Linux and an accessible sound device.');return
        try:
            if self.snapshot:self.cs=self.snapshot['controls'];self.hw=self.snapshot['state']
            else:self.cs=backend.controls(self.card);self.hw=backend.state(self.card)
            self.draw_pages()
        except Exception as exc:self.error(exc)
    def draw_pages(self):
        selected=max(0,self.nav.currentRow());self.nav.clear()
        while self.pages.count():w=self.pages.widget(0);self.pages.removeWidget(w);w.deleteLater()
        direct=self.hw.get('direct',False)
        master=next((c for c in self.cs if c['name']=='Master Playback Switch'),None)
        front=next((c for c in self.cs if c['name']=='Front Playback Switch'),None)
        muted=bool((master and 'off' in master['values']) or (front and 'off' in front['values']))
        mode='384 kHz / 32-bit PCM active' if direct else 'Standard ALSA playback'
        self.banner.setText(('OFFLINE SNAPSHOT · ' if self.snapshot else '')+mode+(' · MASTER OR FRONT MUTED' if muted else ''))
        self.banner.setObjectName('muted' if muted else '')
        for name in ('Playback','Acoustic effects','Equalizer','Recording','Mixer','Profiles','Driver status','Other Windows features'):
            self.nav.addItem(name);scroll=QScrollArea();scroll.setWidgetResizable(True);page=QWidget();lay=QVBoxLayout(page)
            title=QLabel(name);title.setObjectName('title');lay.addWidget(title)
            if name=='Playback':
                lay.addWidget(hint('Output, amplifier preset and DAC filter are separate controls. Changing sample rate does not set amplifier gain.'))
                toggle=QPushButton('Stop Direct mode' if direct else 'Start Direct mode')
                try:
                    configured=json.loads(Path('/usr/lib/ae5-direct/installation.json').read_text())
                    same_device=configured['pci'] in Path(self.card['device_path']).parts
                except (OSError,ValueError,KeyError):same_device=False
                toggle.setEnabled(not self.snapshot and self.card.get('vendor')=='0x11020011' and
                                  self.card.get('subsystem')=='0x11020051' and
                                  same_device and
                                  Path('/usr/lib/ae5-direct/lifecycle.py').exists())
                toggle.clicked.connect(lambda checked=False:self.lifecycle('stop' if direct else 'start'));lay.addWidget(toggle)
                lay.addWidget(hint('Direct 384 kHz requires the configured driver integration. Install/configure it separately; opening this panel does not activate it.'))
            if name in ('Acoustic effects','Equalizer','Recording'):
                lay.addWidget(hint('Direct mode bypasses processing. Effects are unavailable here while Direct mode is active.' if direct else
                    'These controls use the codec DSP. Enable the matching effect master to hear processing.'))
            if name=='Equalizer':
                bands=sorted([c for c in self.cs if c['name'].startswith('EQ Band')],key=lambda c:int(re_band(c['name'])))
                if bands:
                    curve=Curve(bands,not direct and not self.snapshot)
                    curve.changed.connect(lambda change,b=bands:self.write(b[change[0]],[str(change[1])]))
                    lay.addWidget(curve)
                lay.addWidget(hint('The driver exposes ten band indices. Its dB values are shown below. The curve is not a measured frequency response.'))
            for c in self.cs:
                if c['iface']=='MIXER' and group(c)==name:lay.addWidget(self.control_row(c,direct))
            if name=='Profiles':self.profile_page(lay)
            if name=='Driver status':
                lay.addWidget(hint(self.hw.get('evidence','')))
                detail=QLabel(json.dumps(self.hw,indent=2));detail.setTextInteractionFlags(Qt.TextSelectableByMouse);detail.setWordWrap(True);lay.addWidget(detail)
                lay.addWidget(hint(f"AegAudio {__version__}. Driver activation and reboot behavior of this release are untested; this page does not certify audible output."))
            if name=='Other Windows features':
                lay.addWidget(hint('Aurora RGB: no supported ALSA lighting control in the captured driver. No raw register writer is provided.'))
                lay.addWidget(hint('Scout Mode, Dolby Digital Live / DTS Connect encoding, Creative account sync, automatic vendor firmware updates, per-speaker distance calibration and Windows-specific mic EQ are not implemented.'))
                lay.addWidget(hint('ALSA IEC958 controls do not provide a Dolby/DTS encoder or prove a physical SPDIF input exists. These are capability gaps, not functioning toggles.'))
                lay.addWidget(hint('This is an independent Linux application. It contains no Creative Windows executable code, artwork, logos or proprietary profile library.'))
            lay.addStretch();scroll.setWidget(page);self.pages.addWidget(scroll)
        self.nav.setCurrentRow(min(selected,self.pages.count()-1))
    def control_row(self,c,direct):
        frame=QFrame();frame.setObjectName('row');row=QVBoxLayout(frame);line=QHBoxLayout()
        line.addWidget(QLabel(c['name']),1);editors=[]
        editable=(not self.snapshot and c['writable'] and c['type'] in ('BOOLEAN','INTEGER','ENUMERATED'))
        if direct and (c['name'] in backend.PROTECTED or group(c) in ('Acoustic effects','Equalizer','Recording')):editable=False
        for i,v in enumerate(c['values'][:16]):
            if c['type']=='BOOLEAN':w=QCheckBox('L' if i==0 and c['count']==2 else 'R' if i==1 and c['count']==2 else 'On');w.setChecked(v=='on')
            elif c['type']=='ENUMERATED':w=QComboBox();w.addItems(c['items']);w.setCurrentIndex(int(v))
            elif c['type']=='INTEGER':w=QSpinBox();w.setRange(c['min'],c['max']);w.setValue(int(v))
            else:w=QLabel(v)
            w.setEnabled(editable);editors.append(w);line.addWidget(w)
        apply=QPushButton('Apply');apply.setEnabled(editable);line.addWidget(apply)
        def submit():
            values=[('on' if w.isChecked() else 'off') if c['type']=='BOOLEAN' else
                    str(w.currentIndex()) if c['type']=='ENUMERATED' else str(w.value()) for w in editors]
            self.write(c,values)
        apply.clicked.connect(submit);row.addLayout(line)
        if 'db_min' in c:
            row.addWidget(hint('Current level: '+', '.join(f"{c['db_min']+(int(v)-c.get('min',0))*c['db_step']:+.1f} dB" for v in c['values'])))
        if c['name']=='AE-5: Headphone Gain':row.addWidget(hint('Amplifier gain is excluded from saved profiles. An explicit confirmation is required to change it.'))
        return frame
    def write(self,c,values):
        if self.snapshot:return
        protected=c['name'] in backend.PROTECTED
        if protected and QMessageBox.question(self,'Change hardware setting',f"Change {c['name']} from {c['values']} to {values}? This can change output level or route.")!=QMessageBox.Yes:return
        try:
            backend.set_control(self.card,c,values,allow_protected=protected)
            self.footer.setText(c['name']+' updated; ALSA readback matched.');self.refresh()
        except Exception as exc:self.error(exc);self.refresh()
    def profile_page(self,lay):
        lay.addWidget(hint('Profiles save named ALSA controls. They exclude headphone gain, output selection and speaker configuration. Applying a profile can change volumes and mutes.'))
        save=QPushButton('Export current profile…');save.clicked.connect(self.save_profile);lay.addWidget(save)
        load=QPushButton('Import and review profile…');load.setEnabled(not self.snapshot);load.clicked.connect(self.load_profile);lay.addWidget(load)
        lay.addWidget(hint('Profile files are portable between matching codec/subsystem IDs. No profile is automatically loaded or applied on startup.'))
    def save_profile(self):
        path,_=QFileDialog.getSaveFileName(self,'Export profile','audio-profile.json','JSON (*.json)')
        if path:
            try:Path(path).write_text(json.dumps(backend.profile(self.card,self.cs),indent=2))
            except Exception as exc:self.error(exc)
    def load_profile(self):
        path,_=QFileDialog.getOpenFileName(self,'Import profile','','JSON (*.json)')
        if not path:return
        try:
            if Path(path).stat().st_size>1_000_000:raise ValueError('Profile is too large.')
            plan=backend.profile_plan(self.card,json.loads(Path(path).read_text()))
            if self.hw.get('direct'):raise ValueError('Stop Direct mode before applying a processing profile.')
            dialog=QMessageBox(self);dialog.setWindowTitle('Review profile changes');dialog.setText(f'Apply {len(plan)} control changes?')
            dialog.setDetailedText('\n'.join(f"{c['name']}: {c['values']} → {v}" for c,v in plan))
            dialog.setStandardButtons(QMessageBox.Yes|QMessageBox.No);dialog.setDefaultButton(QMessageBox.No)
            if dialog.exec()==QMessageBox.Yes:backend.apply_profile(self.card,plan);self.refresh()
        except Exception as exc:self.error(exc)
    def lifecycle(self,action):
        if self.process:return
        if QMessageBox.question(self,'Direct mode transition','This explicitly stops and restarts the configured audio services. Continue?')!=QMessageBox.Yes:return
        p=QProcess(self);self.process=p
        p.finished.connect(self.lifecycle_done);p.errorOccurred.connect(self.lifecycle_error)
        p.start('pkexec',['/usr/bin/python3','/usr/lib/ae5-direct/lifecycle.py',action])
        self.footer.setText('Waiting for authorization and the requested service transition…')
    def lifecycle_error(self,error):
        if self.process and self.process.state()==QProcess.NotRunning:
            p=self.process;self.process=None;p.deleteLater()
        self.footer.setText('Could not launch Direct mode authorization. No mixer command was sent by the panel.')
    def lifecycle_done(self,exit_code,status):
        p=self.process;self.process=None
        if p is None:return
        if exit_code:self.error(bytes(p.readAllStandardError()).decode(errors='replace') or 'Direct mode transition failed.')
        else:self.footer.setText('Service command completed. Check current device status.');self.refresh()
        p.deleteLater()

def re_band(name):return name.split('Band',1)[1].split()[0]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--snapshot',type=Path)
    args=parser.parse_args();snapshot=json.loads(args.snapshot.read_text()) if args.snapshot else None
    app=QApplication(sys.argv[:1]);app.setStyle('Fusion');app.setStyleSheet(STYLE)
    window=Window(snapshot);window.show();return app.exec()

if __name__=='__main__':raise SystemExit(main())
